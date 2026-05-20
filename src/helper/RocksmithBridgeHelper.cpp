#include "RocksmithBridge/CoreAudioDeviceUtils.h"
#include "RocksmithBridge/SharedRingBuffer.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr UInt32 kMaxCaptureFrames = 16384;

struct CaptureSession {
    AudioUnit unit = nullptr;
    AudioBufferList* bufferList = nullptr;
    UInt32 activePlayerCount = rsbridge::kDefaultActivePlayerCount;
    rsbridge::SharedRing rings[rsbridge::kBridgePlayerCount];
    float* splitBuffers[rsbridge::kBridgePlayerCount] = {};
    std::atomic<OSStatus> renderError{noErr};
};

struct SourceSelection {
    AudioObjectID deviceID = kAudioObjectUnknown;
    rsbridge::InputDeviceInfo info;
    UInt32 channel = 1;
};

UInt32 parseEnvUInt(const char* name, UInt32 fallback, UInt32 minValue, UInt32 maxValue) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    unsigned long value = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0') {
        std::fprintf(stderr, "Ignoring invalid %s=%s\n", name, raw);
        return fallback;
    }
    if (value < minValue) {
        value = minValue;
    }
    if (value > maxValue) {
        value = maxValue;
    }
    return static_cast<UInt32>(value);
}

rsbridge::BridgeConfig loadEffectiveConfig(bool* loadedConfig = nullptr) {
    rsbridge::BridgeConfig config;
    bool loaded = rsbridge::loadConfig(config);
    if (!loaded) {
        config.sourceBufferFrames = parseEnvUInt("RSB_MOTU_BUFFER_FRAMES",
                                                 rsbridge::kDefaultBufferFrames,
                                                 rsbridge::kMinBufferFrames,
                                                 rsbridge::kMaxTargetLatencyFrames);
        config.targetLatencyFrames = parseEnvUInt("RSB_TARGET_LATENCY_FRAMES",
                                                  rsbridge::kDefaultTargetLatencyFrames,
                                                  rsbridge::kMinBufferFrames,
                                                  rsbridge::kMaxTargetLatencyFrames);
        config.virtualBufferFrames = rsbridge::kDefaultBufferFrames;
    }
    if (loadedConfig != nullptr) {
        *loadedConfig = loaded;
    }
    return config;
}

bool sameRuntimeConfig(const rsbridge::BridgeConfig& a, const rsbridge::BridgeConfig& b) {
    return a.sourceUID == b.sourceUID &&
           a.sourceChannel == b.sourceChannel &&
           a.activePlayerCount == b.activePlayerCount &&
           a.sourceBufferFrames == b.sourceBufferFrames &&
           a.targetLatencyFrames == b.targetLatencyFrames &&
           a.virtualBufferFrames == b.virtualBufferFrames;
}

bool deviceIsAlive(AudioObjectID deviceID) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsAlive,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 alive = 0;
    UInt32 size = sizeof(alive);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &alive);
    return status == noErr && alive != 0;
}

void requestDeviceBufferFrames(AudioObjectID deviceID, UInt32 frames, const std::string& deviceName) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    Boolean settable = false;
    OSStatus status = AudioObjectIsPropertySettable(deviceID, &address, &settable);
    if (status != noErr || !settable) {
        std::fprintf(stderr, "Source buffer size is not settable for %s; continuing with current device buffer.\n",
                     deviceName.c_str());
        return;
    }
    UInt32 clamped = rsbridge::clampFrames(frames);
    status = AudioObjectSetPropertyData(deviceID, &address, 0, nullptr, sizeof(clamped), &clamped);
    if (status != noErr) {
        std::fprintf(stderr, "Unable to set %s buffer to %u frames (OSStatus %d); continuing.\n",
                     deviceName.c_str(), clamped, static_cast<int>(status));
        return;
    }
    std::fprintf(stderr, "Requested source buffer: %u frames (%.2f ms at 48 kHz)\n",
                 clamped, static_cast<double>(clamped) * 1000.0 / static_cast<double>(rsbridge::kSampleRate));
}

void requestVirtualBufferFrames(UInt32 frames) {
    const char* uids[] = {rsbridge::kVirtualDeviceUID, rsbridge::kVirtualDevice2UID};
    for (const char* uid : uids) {
        AudioObjectID deviceID = rsbridge::deviceForUIDString(uid);
        if (deviceID == kAudioObjectUnknown) {
            continue;
        }
        AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        Boolean settable = false;
        OSStatus status = AudioObjectIsPropertySettable(deviceID, &address, &settable);
        if (status != noErr || !settable) {
            continue;
        }
        UInt32 clamped = rsbridge::clampFrames(frames);
        AudioObjectSetPropertyData(deviceID, &address, 0, nullptr, sizeof(clamped), &clamped);
    }
}

SourceSelection resolveSource(const rsbridge::BridgeConfig& config) {
    SourceSelection selection;
    if (!config.sourceUID.empty()) {
        selection.deviceID = rsbridge::deviceForUIDString(config.sourceUID);
        selection.info = rsbridge::infoForDevice(selection.deviceID);
        selection.channel = config.sourceChannel == 0 ? 1 : config.sourceChannel;
    } else {
        for (const auto& device : rsbridge::inputDevices()) {
            if (rsbridge::looksLikeMotuM4(device)) {
                selection.deviceID = device.id;
                selection.info = device;
                selection.channel = 1;
                break;
            }
        }
    }

    if (selection.deviceID == kAudioObjectUnknown || selection.info.inputChannels == 0) {
        return {};
    }
    const uint32_t activePlayers = rsbridge::clampActivePlayerCount(config.activePlayerCount);
    if (!rsbridge::hasInputChannelRange(selection.info, selection.channel, activePlayers)) {
        std::fprintf(stderr, "Configured channels %u-%u are invalid for %s (%u input channels).\n",
                     selection.channel,
                     selection.channel + activePlayers - 1,
                     selection.info.name.c_str(), selection.info.inputChannels);
        return {};
    }
    return selection;
}

OSStatus inputCallback(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timestamp,
                       UInt32 busNumber, UInt32 frameCount, AudioBufferList*) {
    auto* session = static_cast<CaptureSession*>(refCon);
    if (frameCount > kMaxCaptureFrames) {
        session->renderError.store(kAudio_ParamError, std::memory_order_relaxed);
        return kAudio_ParamError;
    }
    session->bufferList->mNumberBuffers = 1;
    session->bufferList->mBuffers[0].mNumberChannels = session->activePlayerCount;
    session->bufferList->mBuffers[0].mDataByteSize = frameCount * session->activePlayerCount * sizeof(float);

    OSStatus status = AudioUnitRender(session->unit, flags, timestamp, busNumber, frameCount, session->bufferList);
    if (status == noErr) {
        const auto* input = static_cast<const float*>(session->bufferList->mBuffers[0].mData);
        if (session->activePlayerCount == 1) {
            rsbridge::writeMonoFrames(session->rings[0], input, frameCount);
        } else {
            for (UInt32 frame = 0; frame < frameCount; ++frame) {
                for (UInt32 player = 0; player < session->activePlayerCount; ++player) {
                    session->splitBuffers[player][frame] = input[frame * session->activePlayerCount + player];
                }
            }
            for (UInt32 player = 0; player < session->activePlayerCount; ++player) {
                rsbridge::writeMonoFrames(session->rings[player], session->splitBuffers[player], frameCount);
            }
        }
    } else {
        session->renderError.store(status, std::memory_order_relaxed);
    }
    return status;
}

bool configureCapture(CaptureSession& session, const SourceSelection& source, UInt32 sourceBufferFrames) {
    requestDeviceBufferFrames(source.deviceID, sourceBufferFrames, source.info.name);

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr) {
        std::fprintf(stderr, "Unable to find AUHAL component\n");
        return false;
    }
    OSStatus status = AudioComponentInstanceNew(component, &session.unit);
    if (status != noErr) {
        std::fprintf(stderr, "Unable to create AUHAL instance: OSStatus %d\n", static_cast<int>(status));
        return false;
    }

    UInt32 enable = 1;
    UInt32 disable = 0;
    status = AudioUnitSetProperty(session.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                                  &enable, sizeof(enable));
    if (status == noErr) {
        status = AudioUnitSetProperty(session.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                                      &disable, sizeof(disable));
    }
    if (status == noErr) {
        status = AudioUnitSetProperty(session.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                      &source.deviceID, sizeof(source.deviceID));
    }
    if (status != noErr) {
        std::fprintf(stderr, "Unable to bind AUHAL to %s: OSStatus %d\n", source.info.name.c_str(), static_cast<int>(status));
        return false;
    }

    AudioStreamBasicDescription format{};
    format.mSampleRate = rsbridge::kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    format.mBytesPerPacket = sizeof(float) * session.activePlayerCount;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * session.activePlayerCount;
    format.mChannelsPerFrame = session.activePlayerCount;
    format.mBitsPerChannel = 32;

    status = AudioUnitSetProperty(session.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                                  &format, sizeof(format));
    if (status == noErr) {
        SInt32 channelMap[rsbridge::kBridgePlayerCount] = {};
        for (UInt32 player = 0; player < session.activePlayerCount; ++player) {
            channelMap[player] = static_cast<SInt32>(source.channel - 1 + player);
        }
        status = AudioUnitSetProperty(session.unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1,
                                      channelMap, sizeof(SInt32) * session.activePlayerCount);
    }
    if (status == noErr) {
        AURenderCallbackStruct callback{inputCallback, &session};
        status = AudioUnitSetProperty(session.unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                                      &callback, sizeof(callback));
    }
    if (status != noErr) {
        std::fprintf(stderr, "Unable to configure AUHAL capture: OSStatus %d\n", static_cast<int>(status));
        return false;
    }

    session.bufferList = static_cast<AudioBufferList*>(std::calloc(1, sizeof(AudioBufferList)));
    if (session.bufferList == nullptr) {
        std::fprintf(stderr, "Unable to allocate capture buffer list\n");
        return false;
    }
    session.bufferList->mNumberBuffers = 1;
    session.bufferList->mBuffers[0].mNumberChannels = session.activePlayerCount;
    session.bufferList->mBuffers[0].mDataByteSize = kMaxCaptureFrames * session.activePlayerCount * sizeof(float);
    session.bufferList->mBuffers[0].mData = std::calloc(kMaxCaptureFrames * session.activePlayerCount, sizeof(float));
    if (session.bufferList->mBuffers[0].mData == nullptr) {
        std::fprintf(stderr, "Unable to allocate capture buffers\n");
        return false;
    }
    if (session.activePlayerCount > 1) {
        for (uint32_t player = 0; player < session.activePlayerCount; ++player) {
            session.splitBuffers[player] = static_cast<float*>(std::calloc(kMaxCaptureFrames, sizeof(float)));
            if (session.splitBuffers[player] == nullptr) {
                std::fprintf(stderr, "Unable to allocate channel split buffers\n");
                return false;
            }
        }
    }

    status = AudioUnitInitialize(session.unit);
    if (status != noErr) {
        std::fprintf(stderr, "Unable to initialize AUHAL: OSStatus %d\n", static_cast<int>(status));
        return false;
    }
    return true;
}

void cleanup(CaptureSession& session) {
    if (session.unit != nullptr) {
        AudioOutputUnitStop(session.unit);
        AudioUnitUninitialize(session.unit);
        AudioComponentInstanceDispose(session.unit);
        session.unit = nullptr;
    }
    if (session.bufferList != nullptr) {
        std::free(session.bufferList->mBuffers[0].mData);
        std::free(session.bufferList);
        session.bufferList = nullptr;
    }
    for (uint32_t player = 0; player < rsbridge::kBridgePlayerCount; ++player) {
        std::free(session.splitBuffers[player]);
        session.splitBuffers[player] = nullptr;
        rsbridge::closeSharedRing(session.rings[player]);
    }
}

bool ensureRing(UInt32 targetLatencyFrames, UInt32 activePlayerCount) {
    for (uint32_t player = 1; player <= activePlayerCount; ++player) {
        rsbridge::SharedRing ring;
        if (!rsbridge::openSharedRingForPlayer(ring, player, true)) {
            std::fprintf(stderr, "Unable to open shared ring buffer for player %u\n", player);
            return false;
        }
        rsbridge::setTargetLatencyFrames(ring, targetLatencyFrames);
        rsbridge::closeSharedRing(ring);
    }
    return true;
}

void logTransition(std::string& lastMessage, const char* message) {
    if (lastMessage == message) {
        return;
    }
    lastMessage = message;
    std::fprintf(stderr, "%s\n", message);
}

} // namespace

int main() {
    std::fprintf(stderr, "RocksmithBridgeHelper starting.\n");
    std::string lastWaitMessage;
    while (true) {
        bool loadedConfig = false;
        rsbridge::BridgeConfig config = loadEffectiveConfig(&loadedConfig);
        const UInt32 activePlayerCount = rsbridge::clampActivePlayerCount(config.activePlayerCount);
        if (!ensureRing(config.targetLatencyFrames, activePlayerCount)) {
            logTransition(lastWaitMessage, "Shared ring is unavailable; retrying.");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        requestVirtualBufferFrames(config.virtualBufferFrames);

        SourceSelection source = resolveSource(config);
        if (source.deviceID == kAudioObjectUnknown) {
            logTransition(lastWaitMessage,
                          loadedConfig ? "Configured input source is unavailable; retrying."
                                       : "No config exists and MOTU M4 was not found; retrying.");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        CaptureSession session;
        session.activePlayerCount = activePlayerCount;
        bool ringsReady = true;
        for (uint32_t player = 1; player <= activePlayerCount; ++player) {
            if (!rsbridge::openSharedRingForPlayer(session.rings[player - 1], player, false)) {
                ringsReady = false;
                break;
            }
        }
        if (!ringsReady) {
            logTransition(lastWaitMessage, "Shared ring disappeared; retrying.");
            cleanup(session);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (!configureCapture(session, source, config.sourceBufferFrames)) {
            cleanup(session);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        OSStatus status = AudioOutputUnitStart(session.unit);
        if (status != noErr) {
            std::fprintf(stderr, "Unable to start AUHAL capture: OSStatus %d\n", static_cast<int>(status));
            cleanup(session);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::fprintf(stderr, "Streaming %s channels %u-%u at 48 kHz mono for %u active player(s).\n",
                     source.info.name.c_str(),
                     source.channel,
                     source.channel + activePlayerCount - 1,
                     activePlayerCount);
        lastWaitMessage.clear();

        rsbridge::BridgeConfig activeConfig = config;
        while (deviceIsAlive(source.deviceID) && session.renderError.load(std::memory_order_relaxed) == noErr) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
            rsbridge::BridgeConfig nextConfig = loadEffectiveConfig();
            if (!sameRuntimeConfig(activeConfig, nextConfig)) {
                std::fprintf(stderr, "Config changed; reconnecting capture.\n");
                break;
            }
        }

        OSStatus renderError = session.renderError.load(std::memory_order_relaxed);
        if (renderError != noErr) {
            std::fprintf(stderr, "Capture render error %d; reconnecting.\n", static_cast<int>(renderError));
        } else if (!deviceIsAlive(source.deviceID)) {
            std::fprintf(stderr, "Source device disappeared; reconnecting.\n");
        }
        cleanup(session);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
