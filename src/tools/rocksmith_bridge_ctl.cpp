#include "RocksmithBridge/CoreAudioDeviceUtils.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

const CFStringRef kVirtualDeviceUID = CFSTR("com.vhusso.rocksmithbridge.device");
const CFStringRef kVirtualDevice2UID = CFSTR("com.vhusso.rocksmithbridge.device.2");
const CFStringRef kAggregateUID = CFSTR("com.vhusso.rocksmithbridge.aggregate");
const CFStringRef kAggregate2UID = CFSTR("com.vhusso.rocksmithbridge.aggregate.2");
const CFStringRef kAggregateName = CFSTR("Rocksmith USB Guitar Adapter 1");
const CFStringRef kAggregate2Name = CFSTR("Rocksmith USB Guitar Adapter 2");
constexpr char kInstalledDriverPath[] =
    "/Library/Audio/Plug-Ins/HAL/RocksmithMotuBridge.driver/Contents/MacOS/RocksmithMotuBridge";

void check(OSStatus status, const char* message) {
    if (status != noErr) {
        std::fprintf(stderr, "%s: OSStatus %d\n", message, static_cast<int>(status));
        std::exit(1);
    }
}

UInt32 parseUIntArg(const char* raw, UInt32 minValue, UInt32 maxValue, const char* label) {
    char* end = nullptr;
    unsigned long value = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0') {
        std::fprintf(stderr, "Invalid %s: %s\n", label, raw);
        std::exit(64);
    }
    if (value < minValue || value > maxValue) {
        std::fprintf(stderr, "%s must be between %u and %u\n", label, minValue, maxValue);
        std::exit(64);
    }
    return static_cast<UInt32>(value);
}

bool fileExists(const char* path) {
    struct stat st {};
    return stat(path, &st) == 0;
}

rsbridge::BridgeConfig loadConfigOrDefaults(bool* loaded = nullptr) {
    rsbridge::BridgeConfig config;
    bool ok = rsbridge::loadConfig(config);
    if (loaded != nullptr) {
        *loaded = ok;
    }
    return config;
}

void saveConfigOrExit(const rsbridge::BridgeConfig& config) {
    if (!rsbridge::saveConfig(config)) {
        std::fprintf(stderr, "Unable to write config at %s\n", rsbridge::configPath().c_str());
        std::exit(1);
    }
}

rsbridge::InputDeviceInfo autoMotuSource() {
    for (const auto& device : rsbridge::inputDevices()) {
        if (rsbridge::looksLikeMotuM4(device)) {
            return device;
        }
    }
    return {};
}

rsbridge::InputDeviceInfo resolvedSource(const rsbridge::BridgeConfig& config) {
    if (!config.sourceUID.empty()) {
        return rsbridge::infoForDevice(rsbridge::deviceForUIDString(config.sourceUID));
    }
    return autoMotuSource();
}

void listInputs() {
    for (const auto& device : rsbridge::inputDevices()) {
        std::printf("%u\tchannels:%u\trate:%.0f\t%s\t%s\n",
                    device.id,
                    device.inputChannels,
                    device.nominalSampleRate,
                    device.name.c_str(),
                    device.uid.c_str());
    }
}

const char* yesNo(bool value) {
    return value ? "yes" : "no";
}

std::string readPrompt(const char* prompt) {
    std::printf("%s", prompt);
    std::fflush(stdout);
    char buffer[256] = {};
    if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) {
        return {};
    }
    std::string value = buffer;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

void printConfig() {
    bool loaded = false;
    rsbridge::BridgeConfig config = loadConfigOrDefaults(&loaded);
    rsbridge::InputDeviceInfo source = resolvedSource(config);
    std::printf("config-path: %s\n", rsbridge::configPath().c_str());
    std::printf("config-present: %s\n", loaded ? "yes" : "no");
    std::printf("source-mode: %s\n", config.sourceUID.empty() ? "auto MOTU M4 input 1" : "configured");
    std::printf("source-uid: %s\n", config.sourceUID.empty() ? "(auto)" : config.sourceUID.c_str());
    std::printf("source-channel: %u\n", config.sourceChannel == 0 ? 1 : config.sourceChannel);
    std::printf("source-buffer-frames: %u\n", config.sourceBufferFrames);
    std::printf("bridge-latency-frames: %u\n", config.targetLatencyFrames);
    std::printf("virtual-buffer-frames: %u\n", config.virtualBufferFrames);
    if (source.id == kAudioObjectUnknown || source.inputChannels == 0) {
        std::printf("resolved-source: unavailable\n");
    } else {
        std::printf("resolved-source: %s (%s), channels:%u, rate:%.0f\n",
                    source.name.c_str(),
                    source.uid.c_str(),
                    source.inputChannels,
                    source.nominalSampleRate);
    }
}

bool requestVirtualBufferFrames(UInt32 frames, bool noisy) {
    const CFStringRef uids[] = {kVirtualDeviceUID, kVirtualDevice2UID};
    bool any = false;
    for (CFStringRef uid : uids) {
        AudioObjectID source = rsbridge::deviceForUID(uid);
        if (source == kAudioObjectUnknown) {
            continue;
        }
        AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        Boolean settable = false;
        OSStatus status = AudioObjectIsPropertySettable(source, &address, &settable);
        if (status != noErr || !settable) {
            continue;
        }
        status = AudioObjectSetPropertyData(source, &address, 0, nullptr, sizeof(frames), &frames);
        if (status == noErr) {
            any = true;
        }
    }
    if (!any && noisy) {
        std::fprintf(stderr, "No virtual sources accepted buffer size changes. Install the driver and restart coreaudiod first.\n");
        return false;
    }
    return any;
}

bool requestRingLatency(UInt32 frames, bool noisy) {
    bool any = false;
    for (uint32_t player = 1; player <= rsbridge::kBridgePlayerCount; ++player) {
        rsbridge::SharedRing ring;
        rsbridge::SharedRingOpenError error = rsbridge::SharedRingOpenError::none;
        errno = 0;
        if (!rsbridge::openSharedRingForPlayer(ring, player, false, &error)) {
            if (noisy) {
                std::fprintf(stderr, "Shared ring %u is unavailable (%s). Start the helper first.\n",
                             player,
                             rsbridge::sharedRingOpenErrorMessage(error));
            }
            continue;
        }
        rsbridge::setTargetLatencyFrames(ring, frames);
        rsbridge::closeSharedRing(ring);
        any = true;
    }
    return any;
}

void status() {
    rsbridge::BridgeConfig config = loadConfigOrDefaults();
    rsbridge::InputDeviceInfo sourceInfo = resolvedSource(config);
    const UInt32 firstChannel = config.sourceUID.empty() ? 1 : config.sourceChannel;
    const bool sourceReady = sourceInfo.id != kAudioObjectUnknown &&
                             rsbridge::hasInputChannelRange(sourceInfo, firstChannel);
    bool ringsReady = true;
    bool helperActive = false;
    double maxPeak = 0.0;
    for (uint32_t player = 1; player <= rsbridge::kBridgePlayerCount; ++player) {
        rsbridge::SharedRing ring;
        if (!rsbridge::openSharedRingForPlayer(ring, player, false, nullptr, rsbridge::SharedRingAccess::readOnly)) {
            ringsReady = false;
            continue;
        }
        helperActive = helperActive || ring.header->heartbeat.load(std::memory_order_acquire) > 0;
        const double peak = static_cast<double>(ring.header->inputPeakPpm.load(std::memory_order_acquire)) / 1000000.0;
        if (peak > maxPeak) {
            maxPeak = peak;
        }
        rsbridge::closeSharedRing(ring);
    }

    AudioObjectID virtualSource1 = rsbridge::deviceForUID(kVirtualDeviceUID);
    const bool virtualReady = virtualSource1 != kAudioObjectUnknown &&
                              rsbridge::channelCount(virtualSource1, kAudioObjectPropertyScopeInput) == 1;
    AudioObjectID aggregate1 = rsbridge::deviceForUID(kAggregateUID);
    const bool aggregateReady = aggregate1 != kAudioObjectUnknown &&
                                rsbridge::channelCount(aggregate1, kAudioObjectPropertyScopeInput) == 1;
    const bool ready = fileExists(kInstalledDriverPath) && sourceReady && ringsReady &&
                       helperActive && virtualReady && aggregateReady;
    std::printf("ready: %s\n", yesNo(ready));
    std::printf("source: %s\n", sourceReady ? sourceInfo.name.c_str() : "unavailable");
    if (sourceReady) {
        std::printf("source-channels: %u-%u\n", firstChannel, firstChannel + rsbridge::kBridgePlayerCount - 1);
    }
    std::printf("buffers: source=%u bridge=%u virtual=%u\n",
                config.sourceBufferFrames, config.targetLatencyFrames, config.virtualBufferFrames);
    std::printf("helper: %s\n", helperActive ? "streaming" : "not streaming");
    std::printf("virtual-source-1: %s\n", yesNo(virtualReady));
    std::printf("rocksmith-aggregate-1: %s\n", yesNo(aggregateReady));
    std::printf("input-level: %.4f\n\n", maxPeak);

    for (uint32_t player = 1; player <= 2; ++player) {
        rsbridge::SharedRing ring;
        rsbridge::SharedRingOpenError error = rsbridge::SharedRingOpenError::none;
        errno = 0;
        if (!rsbridge::openSharedRingForPlayer(ring, player, false, &error)) {
            std::printf("shared-ring-%u: unavailable (%s", player, rsbridge::sharedRingOpenErrorMessage(error));
            if (errno != 0) {
                std::printf(": %s", std::strerror(errno));
            }
            std::printf(")\n");
        } else {
            const UInt32 target = ring.header->targetLatencyFrames.load(std::memory_order_acquire);
            std::printf("shared-ring-%u: ok\n", player);
            std::printf("ring-%u-target-latency-frames: %u (%.2f ms at 48 kHz)\n",
                        player,
                        target,
                        static_cast<double>(target) * 1000.0 / static_cast<double>(rsbridge::kSampleRate));
            std::printf("ring-%u-write-frame: %llu\n", player,
                        static_cast<unsigned long long>(ring.header->writeFrame.load(std::memory_order_acquire)));
            std::printf("ring-%u-heartbeats: %llu\n", player,
                        static_cast<unsigned long long>(ring.header->heartbeat.load(std::memory_order_acquire)));
            const UInt32 peakPpm = ring.header->inputPeakPpm.load(std::memory_order_acquire);
            std::printf("ring-%u-input-peak: %.4f\n", player, static_cast<double>(peakPpm) / 1000000.0);
            std::printf("ring-%u-driver-read-calls: %llu\n", player,
                        static_cast<unsigned long long>(ring.header->driverReadCalls.load(std::memory_order_acquire)));
            std::printf("ring-%u-driver-read-frames: %llu\n", player,
                        static_cast<unsigned long long>(ring.header->driverReadFrames.load(std::memory_order_acquire)));
            std::printf("ring-%u-underruns: %llu\n", player,
                        static_cast<unsigned long long>(ring.header->underruns.load(std::memory_order_acquire)));
            std::printf("ring-%u-resyncs: %llu\n", player,
                        static_cast<unsigned long long>(ring.header->overruns.load(std::memory_order_acquire)));
            rsbridge::closeSharedRing(ring);
        }
    }

    const CFStringRef virtualUIDs[] = {kVirtualDeviceUID, kVirtualDevice2UID};
    const char* virtualLabels[] = {"virtual-source-1", "virtual-source-2"};
    for (uint32_t i = 0; i < rsbridge::kBridgePlayerCount; ++i) {
        AudioObjectID source = rsbridge::deviceForUID(virtualUIDs[i]);
        std::printf("%s: %s\n", virtualLabels[i], source == kAudioObjectUnknown ? "missing" : "visible");
        if (source != kAudioObjectUnknown) {
            std::printf("%s-input-channels: %u\n", virtualLabels[i],
                        rsbridge::channelCount(source, kAudioObjectPropertyScopeInput));
        }
    }
    const CFStringRef aggregateUIDs[] = {kAggregateUID, kAggregate2UID};
    const char* aggregateLabels[] = {"rocksmith-aggregate-1", "rocksmith-aggregate-2"};
    for (uint32_t i = 0; i < rsbridge::kBridgePlayerCount; ++i) {
        AudioObjectID aggregate = rsbridge::deviceForUID(aggregateUIDs[i]);
        std::printf("%s: %s\n", aggregateLabels[i], aggregate == kAudioObjectUnknown ? "missing" : "visible");
        if (aggregate != kAudioObjectUnknown) {
            std::printf("%s-input-channels: %u\n", aggregateLabels[i],
                        rsbridge::channelCount(aggregate, kAudioObjectPropertyScopeInput));
        }
    }
}

void setBridgeLatency(const char* rawFrames) {
    UInt32 frames = parseUIntArg(rawFrames, rsbridge::kMinBufferFrames, rsbridge::kMaxTargetLatencyFrames, "frame count");
    rsbridge::BridgeConfig config = loadConfigOrDefaults();
    config.targetLatencyFrames = frames;
    saveConfigOrExit(config);
    requestRingLatency(frames, true);
    std::printf("Set bridge safety latency to %u frames (%.2f ms at 48 kHz)\n",
                frames,
                static_cast<double>(frames) * 1000.0 / static_cast<double>(rsbridge::kSampleRate));
}

void setVirtualBuffer(const char* rawFrames) {
    UInt32 frames = parseUIntArg(rawFrames, rsbridge::kMinBufferFrames, rsbridge::kMaxTargetLatencyFrames, "frame count");
    rsbridge::BridgeConfig config = loadConfigOrDefaults();
    config.virtualBufferFrames = frames;
    saveConfigOrExit(config);
    if (!requestVirtualBufferFrames(frames, true)) {
        std::exit(2);
    }
    std::printf("Requested virtual source buffer size: %u frames (%.2f ms at 48 kHz)\n",
                frames,
                static_cast<double>(frames) * 1000.0 / static_cast<double>(rsbridge::kSampleRate));
}

void setBuffers(const char* rawFrames) {
    UInt32 frames = parseUIntArg(rawFrames, rsbridge::kMinBufferFrames, rsbridge::kMaxTargetLatencyFrames, "frame count");
    rsbridge::BridgeConfig config = loadConfigOrDefaults();
    config.sourceBufferFrames = frames;
    config.targetLatencyFrames = frames;
    config.virtualBufferFrames = frames;
    saveConfigOrExit(config);
    requestRingLatency(frames, false);
    requestVirtualBufferFrames(frames, false);
    std::printf("Persisted source, bridge, and virtual buffers at %u frames (%.2f ms at 48 kHz)\n",
                frames,
                static_cast<double>(frames) * 1000.0 / static_cast<double>(rsbridge::kSampleRate));
}

void setSource(const char* uid, const char* rawChannel) {
    UInt32 channel = parseUIntArg(rawChannel, 1, 1024, "channel");
    rsbridge::InputDeviceInfo info = rsbridge::infoForDevice(rsbridge::deviceForUIDString(uid));
    if (info.id == kAudioObjectUnknown || info.inputChannels == 0) {
        std::fprintf(stderr, "Input device UID not found: %s\n", uid);
        std::exit(2);
    }
    if (!rsbridge::hasInputChannelRange(info, channel)) {
        std::fprintf(stderr, "%s has %u input channels; channels %u-%u are invalid.\n",
                     info.name.c_str(), info.inputChannels, channel, channel + rsbridge::kBridgePlayerCount - 1);
        std::exit(64);
    }
    rsbridge::BridgeConfig config = loadConfigOrDefaults();
    config.sourceUID = uid;
    config.sourceChannel = channel;
    saveConfigOrExit(config);
    std::printf("Set source to %s channels %u-%u\n",
                info.name.c_str(),
                channel,
                channel + rsbridge::kBridgePlayerCount - 1);
}

void chooseSource() {
    std::vector<rsbridge::InputDeviceInfo> devices = rsbridge::inputDevices();
    std::vector<rsbridge::InputDeviceInfo> eligible;
    for (const auto& device : devices) {
        if (device.inputChannels >= rsbridge::kBridgePlayerCount) {
            eligible.push_back(device);
        }
    }
    if (eligible.empty()) {
        std::fprintf(stderr, "No input device has the required adjacent channel pair.\n");
        std::exit(2);
    }

    std::printf("Input devices with at least %u channels:\n", rsbridge::kBridgePlayerCount);
    for (size_t i = 0; i < eligible.size(); ++i) {
        const auto& device = eligible[i];
        std::printf("  %zu. %s  channels:%u  rate:%.0f\n",
                    i + 1, device.name.c_str(), device.inputChannels, device.nominalSampleRate);
        std::printf("     %s\n", device.uid.c_str());
    }

    std::string rawDevice = readPrompt("Device number: ");
    UInt32 deviceIndex = parseUIntArg(rawDevice.c_str(), 1, static_cast<UInt32>(eligible.size()), "device number");
    const auto& selected = eligible[deviceIndex - 1];

    std::string rawChannel = readPrompt("First channel for player 1 [1]: ");
    UInt32 channel = rawChannel.empty() ? 1 : parseUIntArg(rawChannel.c_str(), 1, selected.inputChannels, "channel");
    if (!rsbridge::hasInputChannelRange(selected, channel)) {
        std::fprintf(stderr, "%s has %u input channels; channels %u-%u are invalid.\n",
                     selected.name.c_str(), selected.inputChannels, channel, channel + rsbridge::kBridgePlayerCount - 1);
        std::exit(64);
    }

    rsbridge::BridgeConfig config = loadConfigOrDefaults();
    config.sourceUID = selected.uid;
    config.sourceChannel = channel;
    saveConfigOrExit(config);
    std::printf("Set source to %s channels %u-%u\n",
                selected.name.c_str(), channel, channel + rsbridge::kBridgePlayerCount - 1);
}

void destroyAggregateIfPresent(CFStringRef uid) {
    AudioObjectID aggregate = rsbridge::deviceForUID(uid);
    if (aggregate == kAudioObjectUnknown) {
        return;
    }
    check(AudioHardwareDestroyAggregateDevice(aggregate), "Unable to destroy existing aggregate");
}

void destroyAggregatesIfPresent() {
    destroyAggregateIfPresent(kAggregateUID);
    destroyAggregateIfPresent(kAggregate2UID);
}

void createOneAggregate(CFStringRef sourceUID, CFStringRef aggregateUID, CFStringRef aggregateName) {
    AudioObjectID source = rsbridge::deviceForUID(sourceUID);
    if (source == kAudioObjectUnknown) {
        std::fprintf(stderr, "Virtual device is not visible yet. Install the driver and restart coreaudiod first.\n");
        std::exit(2);
    }
    if (rsbridge::channelCount(source, kAudioObjectPropertyScopeInput) != 1) {
        std::fprintf(stderr, "Virtual device exists, but it does not report exactly one input channel.\n");
        std::exit(3);
    }

    CFMutableDictionaryRef subDevice = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(subDevice, CFSTR(kAudioSubDeviceUIDKey), sourceUID);

    const void* subDevices[] = {subDevice};
    CFArrayRef subDeviceList = CFArrayCreate(nullptr, subDevices, 1, &kCFTypeArrayCallBacks);

    int isPrivate = 0;
    int isStacked = 0;
    CFNumberRef privateNumber = CFNumberCreate(nullptr, kCFNumberIntType, &isPrivate);
    CFNumberRef stackedNumber = CFNumberCreate(nullptr, kCFNumberIntType, &isStacked);

    CFMutableDictionaryRef aggregate = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceNameKey), aggregateName);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceUIDKey), aggregateUID);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subDeviceList);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), sourceUID);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceClockDeviceKey), sourceUID);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceIsPrivateKey), privateNumber);
    CFDictionarySetValue(aggregate, CFSTR(kAudioAggregateDeviceIsStackedKey), stackedNumber);

    AudioObjectID aggregateID = kAudioObjectUnknown;
    check(AudioHardwareCreateAggregateDevice(aggregate, &aggregateID), "Unable to create aggregate device");
    std::printf("Created %s (AudioObjectID %u)\n", rsbridge::cfStringToStdString(aggregateName).c_str(), aggregateID);

    CFRelease(aggregate);
    CFRelease(stackedNumber);
    CFRelease(privateNumber);
    CFRelease(subDeviceList);
    CFRelease(subDevice);
}

void createPlayerOneAggregate() {
    destroyAggregateIfPresent(kAggregateUID);
    createOneAggregate(kVirtualDeviceUID, kAggregateUID, kAggregateName);
}

void createAllAggregates() {
    destroyAggregatesIfPresent();
    createOneAggregate(kVirtualDeviceUID, kAggregateUID, kAggregateName);
    createOneAggregate(kVirtualDevice2UID, kAggregate2UID, kAggregate2Name);
}

bool doctorCheck(bool condition, const char* ok, const char* fail, bool failIsWarning = false) {
    if (condition) {
        std::printf("[ok] %s\n", ok);
        return true;
    }
    std::printf("[%s] %s\n", failIsWarning ? "warn" : "fail", fail);
    return failIsWarning;
}

void doctor() {
    bool healthy = true;
    healthy &= doctorCheck(fileExists(kInstalledDriverPath),
                           "HAL driver is installed",
                           "HAL driver is not installed; run sudo make install-local");

    const CFStringRef virtualUIDs[] = {kVirtualDeviceUID, kVirtualDevice2UID};
    const CFStringRef aggregateUIDs[] = {kAggregateUID, kAggregate2UID};
    for (uint32_t i = 0; i < rsbridge::kBridgePlayerCount; ++i) {
        AudioObjectID source = rsbridge::deviceForUID(virtualUIDs[i]);
        char ok[96];
        char fail[128];
        std::snprintf(ok, sizeof(ok), "virtual source %d is visible", i + 1);
        std::snprintf(fail, sizeof(fail), "virtual source %d is missing; restart coreaudiod after installing", i + 1);
        healthy &= doctorCheck(source != kAudioObjectUnknown, ok, fail, i > 0);
        if (source != kAudioObjectUnknown) {
            std::snprintf(ok, sizeof(ok), "virtual source %d reports 1 input channel", i + 1);
            std::snprintf(fail, sizeof(fail), "virtual source %d does not report exactly 1 input channel", i + 1);
            healthy &= doctorCheck(rsbridge::channelCount(source, kAudioObjectPropertyScopeInput) == 1, ok, fail, i > 0);
        }

        AudioObjectID aggregate = rsbridge::deviceForUID(aggregateUIDs[i]);
        std::snprintf(ok, sizeof(ok), "Rocksmith aggregate %d is visible", i + 1);
        std::snprintf(fail, sizeof(fail), "Rocksmith aggregate %d is missing; run rocksmith_bridge_ctl repair-aggregate", i + 1);
        if (i > 0) {
            std::snprintf(fail, sizeof(fail), "Rocksmith aggregate %d is missing; run repair-aggregate-all for two-player setup", i + 1);
        }
        healthy &= doctorCheck(aggregate != kAudioObjectUnknown, ok, fail, i > 0);
        if (aggregate != kAudioObjectUnknown) {
            std::snprintf(ok, sizeof(ok), "Rocksmith aggregate %d reports 1 input channel", i + 1);
            std::snprintf(fail, sizeof(fail), "Rocksmith aggregate %d does not report exactly 1 input channel", i + 1);
            healthy &= doctorCheck(rsbridge::channelCount(aggregate, kAudioObjectPropertyScopeInput) == 1, ok, fail, i > 0);
        }
    }

    bool loaded = false;
    rsbridge::BridgeConfig config = loadConfigOrDefaults(&loaded);
    std::printf("[info] config: %s (%s)\n", rsbridge::configPath().c_str(), loaded ? "present" : "using defaults");
    rsbridge::InputDeviceInfo selected = resolvedSource(config);
    healthy &= doctorCheck(selected.id != kAudioObjectUnknown && selected.inputChannels > 0,
                           "configured input source is available",
                           config.sourceUID.empty() ? "MOTU M4 auto-detect did not find an input device"
                                                    : "configured input source is unavailable");
    if (selected.id != kAudioObjectUnknown) {
        UInt32 firstChannel = config.sourceUID.empty() ? 1 : config.sourceChannel;
        healthy &= doctorCheck(rsbridge::hasInputChannelRange(selected, firstChannel),
                               "configured input channel pair is valid",
                               "configured input channel pair is invalid");
        std::printf("[info] source: %s channels %u-%u, channels:%u, rate:%.0f\n",
                    selected.name.c_str(),
                    firstChannel,
                    firstChannel + rsbridge::kBridgePlayerCount - 1,
                    selected.inputChannels,
                    selected.nominalSampleRate);
    }
    std::printf("[info] buffers: source=%u bridge=%u virtual=%u\n",
                config.sourceBufferFrames, config.targetLatencyFrames, config.virtualBufferFrames);

    for (uint32_t player = 1; player <= rsbridge::kBridgePlayerCount; ++player) {
        rsbridge::SharedRing ring;
        rsbridge::SharedRingOpenError error = rsbridge::SharedRingOpenError::none;
        errno = 0;
        if (rsbridge::openSharedRingForPlayer(ring, player, false, &error)) {
            const auto heartbeat = ring.header->heartbeat.load(std::memory_order_acquire);
            const auto reads = ring.header->driverReadCalls.load(std::memory_order_acquire);
            const auto peak = ring.header->inputPeakPpm.load(std::memory_order_acquire);
            std::printf("[ok] shared ring %u is available\n", player);
            std::printf("[info] ring-%u heartbeats=%llu input-peak=%.4f driver-read-calls=%llu\n",
                        player,
                        static_cast<unsigned long long>(heartbeat),
                        static_cast<double>(peak) / 1000000.0,
                        static_cast<unsigned long long>(reads));
            doctorCheck(heartbeat > 0, "helper has written audio callbacks", "helper has not written audio yet", true);
            if (reads == 0) {
                std::printf("[info] driver read counter is zero; read-only driver mappings may leave this unchanged\n");
            }
            rsbridge::closeSharedRing(ring);
        } else {
            healthy = false;
            std::printf("[fail] shared ring %u unavailable (%s); start or reload the helper\n",
                        player,
                        rsbridge::sharedRingOpenErrorMessage(error));
        }
    }
    std::exit(healthy ? 0 : 1);
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s COMMAND\n"
                 "Commands:\n"
                 "  list-inputs\n"
                 "  choose-source\n"
                 "  set-source DEVICE_UID CHANNEL\n"
                 "  get-config\n"
                 "  set-buffers FRAMES\n"
                 "  doctor\n"
                 "  status\n"
                 "  set-bridge-latency FRAMES\n"
                 "  set-virtual-buffer FRAMES\n"
                 "  repair-aggregate | create-aggregate | repair-aggregate-all | destroy-aggregate\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 64;
    }
    if (std::strcmp(argv[1], "list-inputs") == 0 || std::strcmp(argv[1], "list-devices") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        listInputs();
    } else if (std::strcmp(argv[1], "choose-source") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        chooseSource();
    } else if (std::strcmp(argv[1], "set-source") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 64;
        }
        setSource(argv[2], argv[3]);
    } else if (std::strcmp(argv[1], "get-config") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        printConfig();
    } else if (std::strcmp(argv[1], "set-buffers") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 64;
        }
        setBuffers(argv[2]);
    } else if (std::strcmp(argv[1], "doctor") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        doctor();
    } else if (std::strcmp(argv[1], "status") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        status();
    } else if (std::strcmp(argv[1], "set-bridge-latency") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 64;
        }
        setBridgeLatency(argv[2]);
    } else if (std::strcmp(argv[1], "set-virtual-buffer") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 64;
        }
        setVirtualBuffer(argv[2]);
    } else if (std::strcmp(argv[1], "create-aggregate") == 0 || std::strcmp(argv[1], "repair-aggregate") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        createPlayerOneAggregate();
    } else if (std::strcmp(argv[1], "repair-aggregate-all") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        createAllAggregates();
    } else if (std::strcmp(argv[1], "destroy-aggregate") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 64;
        }
        destroyAggregatesIfPresent();
    } else {
        usage(argv[0]);
        return 64;
    }
    return 0;
}
