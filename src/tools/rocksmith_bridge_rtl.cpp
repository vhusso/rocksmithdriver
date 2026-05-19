#include "RocksmithBridge/CoreAudioDeviceUtils.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>
#include <mach/mach_time.h>

#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr UInt32 kMaxFrames = 16384;
constexpr UInt32 kPulseFrames = 512;
constexpr Float64 kMeasureSampleRate = 48000.0;
constexpr float kDefaultPulseAmplitude = 0.50f;
constexpr float kDefaultDetectThreshold = 0.25f;
constexpr float kMinCorrelationPeak = 0.0005f;

struct DeviceSelection {
    AudioObjectID id = kAudioObjectUnknown;
    std::string name;
    std::string uid;
    UInt32 channel = 1;
};

struct MeasureState {
    AudioUnit inputUnit = nullptr;
    AudioUnit outputUnit = nullptr;
    AudioBufferList* inputBuffer = nullptr;
    double ticksPerFrame = 0;
    float pulseAmplitude = kDefaultPulseAmplitude;
    float detectThreshold = kDefaultDetectThreshold;
    std::array<float, kPulseFrames> history {};
    UInt64 inputFrames = 0;
    float patternEnergy = 0;
    UInt64 warmupFrames = 0;
    std::atomic<UInt64> outputFrame{0};
    std::atomic<UInt64> inputCallbacks{0};
    std::atomic<UInt64> outputCallbacks{0};
    std::atomic<UInt64> pulseHostTime{0};
    std::atomic<UInt64> detectedHostTime{0};
    std::atomic<bool> pulseSent{false};
    std::atomic<bool> detected{false};
    std::atomic<float> detectedPeak{0.0f};
    std::atomic<float> observedPeak{0.0f};
    std::atomic<OSStatus> error{noErr};
};

void fail(const char* message, OSStatus status = noErr) {
    if (status == noErr) {
        std::fprintf(stderr, "%s\n", message);
    } else {
        std::fprintf(stderr, "%s: OSStatus %d\n", message, static_cast<int>(status));
    }
    std::exit(1);
}

UInt32 parseUInt(const char* raw, UInt32 minValue, UInt32 maxValue, const char* label) {
    char* end = nullptr;
    unsigned long value = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || value < minValue || value > maxValue) {
        std::fprintf(stderr, "%s must be between %u and %u\n", label, minValue, maxValue);
        std::exit(64);
    }
    return static_cast<UInt32>(value);
}

float parseFloat(const char* raw, float minValue, float maxValue, const char* label) {
    char* end = nullptr;
    float value = std::strtof(raw, &end);
    if (end == raw || *end != '\0' || value < minValue || value > maxValue) {
        std::fprintf(stderr, "%s must be between %.4f and %.4f\n", label, minValue, maxValue);
        std::exit(64);
    }
    return value;
}

float pulsePatternSample(UInt32 index, float amplitude) {
    return ((index / 8) % 2 == 0) ? amplitude : -amplitude;
}

float pulsePatternEnergy(float amplitude) {
    return static_cast<float>(kPulseFrames) * amplitude * amplitude;
}

double nanosPerHostTick() {
    mach_timebase_info_data_t info {};
    mach_timebase_info(&info);
    return static_cast<double>(info.numer) / static_cast<double>(info.denom);
}

AudioStreamBasicDescription monoFloatFormat() {
    AudioStreamBasicDescription format {};
    format.mSampleRate = kMeasureSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;
    return format;
}

AudioUnit makeHalUnit() {
    AudioComponentDescription desc {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr) {
        fail("Unable to find AUHAL component");
    }
    AudioUnit unit = nullptr;
    OSStatus status = AudioComponentInstanceNew(component, &unit);
    if (status != noErr) {
        fail("Unable to create AUHAL instance", status);
    }
    return unit;
}

OSStatus outputCallback(void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp* timestamp,
                        UInt32, UInt32 frameCount, AudioBufferList* ioData) {
    auto* state = static_cast<MeasureState*>(refCon);
    state->outputCallbacks.fetch_add(1, std::memory_order_relaxed);
    auto* output = static_cast<float*>(ioData->mBuffers[0].mData);
    std::memset(output, 0, frameCount * sizeof(float));

    UInt64 baseFrame = state->outputFrame.fetch_add(frameCount, std::memory_order_relaxed);
    if (baseFrame + frameCount > state->warmupFrames && baseFrame < state->warmupFrames + kPulseFrames) {
        UInt32 start = state->warmupFrames > baseFrame ? static_cast<UInt32>(state->warmupFrames - baseFrame) : 0;
        UInt64 pulseEndFrame = state->warmupFrames + kPulseFrames;
        UInt32 end = pulseEndFrame < baseFrame + frameCount ? static_cast<UInt32>(pulseEndFrame - baseFrame) : frameCount;
        if (start < end) {
            if (!state->pulseSent.exchange(true, std::memory_order_acq_rel)) {
                UInt64 pulseHost = timestamp->mHostTime + static_cast<UInt64>(state->ticksPerFrame * start);
                state->pulseHostTime.store(pulseHost, std::memory_order_release);
            }
            for (UInt32 i = start; i < end; ++i) {
                output[i] = pulsePatternSample(static_cast<UInt32>(baseFrame + i - state->warmupFrames),
                                               state->pulseAmplitude);
            }
        }
    }
    return noErr;
}

OSStatus inputCallback(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timestamp,
                       UInt32 busNumber, UInt32 frameCount, AudioBufferList*) {
    auto* state = static_cast<MeasureState*>(refCon);
    state->inputCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (frameCount > kMaxFrames) {
        state->error.store(kAudio_ParamError, std::memory_order_relaxed);
        return kAudio_ParamError;
    }
    state->inputBuffer->mBuffers[0].mDataByteSize = frameCount * sizeof(float);
    OSStatus status = AudioUnitRender(state->inputUnit, flags, timestamp, busNumber, frameCount, state->inputBuffer);
    if (status != noErr) {
        state->error.store(status, std::memory_order_relaxed);
        return status;
    }
    if (!state->pulseSent.load(std::memory_order_acquire) || state->detected.load(std::memory_order_acquire)) {
        return noErr;
    }

    const auto* input = static_cast<const float*>(state->inputBuffer->mBuffers[0].mData);
    for (UInt32 i = 0; i < frameCount; ++i) {
        float sample = std::fabs(input[i]);
        float currentPeak = state->observedPeak.load(std::memory_order_relaxed);
        while (sample > currentPeak &&
               !state->observedPeak.compare_exchange_weak(currentPeak, sample, std::memory_order_relaxed)) {
        }
        state->history[state->inputFrames % kPulseFrames] = input[i];
        ++state->inputFrames;
        if (state->inputFrames >= kPulseFrames) {
            float dot = 0.0f;
            float inputEnergy = 0.0f;
            float windowPeak = 0.0f;
            UInt64 firstFrame = state->inputFrames - kPulseFrames;
            for (UInt32 p = 0; p < kPulseFrames; ++p) {
                float value = state->history[(firstFrame + p) % kPulseFrames];
                float absValue = std::fabs(value);
                if (absValue > windowPeak) {
                    windowPeak = absValue;
                }
                dot += value * pulsePatternSample(p, state->pulseAmplitude);
                inputEnergy += value * value;
            }
            if (windowPeak >= kMinCorrelationPeak && inputEnergy > 0.0f) {
                float score = dot / std::sqrt(inputEnergy * state->patternEnergy);
                if (score >= state->detectThreshold) {
                    UInt64 startOffset = i + 1 >= kPulseFrames ? i + 1 - kPulseFrames : 0;
                    UInt64 detectedHost = timestamp->mHostTime + static_cast<UInt64>(state->ticksPerFrame * startOffset);
                    state->detectedPeak.store(windowPeak, std::memory_order_release);
                    state->detectedHostTime.store(detectedHost, std::memory_order_release);
                    state->detected.store(true, std::memory_order_release);
                    break;
                }
            }
        }
    }
    return noErr;
}

void configureInput(MeasureState& state, const DeviceSelection& input) {
    state.inputUnit = makeHalUnit();
    UInt32 enable = 1;
    UInt32 disable = 0;
    AudioStreamBasicDescription format = monoFloatFormat();
    OSStatus status = AudioUnitSetProperty(state.inputUnit, kAudioOutputUnitProperty_EnableIO,
                                           kAudioUnitScope_Input, 1, &enable, sizeof(enable));
    if (status == noErr) {
        status = AudioUnitSetProperty(state.inputUnit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Output, 0, &disable, sizeof(disable));
    }
    if (status == noErr) {
        status = AudioUnitSetProperty(state.inputUnit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0, &input.id, sizeof(input.id));
    }
    if (status == noErr) {
        status = AudioUnitSetProperty(state.inputUnit, kAudioUnitProperty_StreamFormat,
                                      kAudioUnitScope_Output, 1, &format, sizeof(format));
    }
    if (status == noErr) {
        SInt32 map[] = {static_cast<SInt32>(input.channel - 1)};
        status = AudioUnitSetProperty(state.inputUnit, kAudioOutputUnitProperty_ChannelMap,
                                      kAudioUnitScope_Output, 1, map, sizeof(map));
    }
    if (status == noErr) {
        AURenderCallbackStruct callback {inputCallback, &state};
        status = AudioUnitSetProperty(state.inputUnit, kAudioOutputUnitProperty_SetInputCallback,
                                      kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    }
    if (status != noErr) {
        fail("Unable to configure input AUHAL", status);
    }

    state.inputBuffer = static_cast<AudioBufferList*>(std::calloc(1, sizeof(AudioBufferList)));
    state.inputBuffer->mNumberBuffers = 1;
    state.inputBuffer->mBuffers[0].mNumberChannels = 1;
    state.inputBuffer->mBuffers[0].mDataByteSize = kMaxFrames * sizeof(float);
    state.inputBuffer->mBuffers[0].mData = std::calloc(kMaxFrames, sizeof(float));
    status = AudioUnitInitialize(state.inputUnit);
    if (status != noErr) {
        fail("Unable to initialize input AUHAL", status);
    }
}

void configureOutput(MeasureState& state, const DeviceSelection& output) {
    state.outputUnit = makeHalUnit();
    UInt32 enable = 1;
    UInt32 disable = 0;
    AudioStreamBasicDescription format = monoFloatFormat();
    OSStatus status = AudioUnitSetProperty(state.outputUnit, kAudioOutputUnitProperty_EnableIO,
                                           kAudioUnitScope_Output, 0, &enable, sizeof(enable));
    if (status == noErr) {
        status = AudioUnitSetProperty(state.outputUnit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Input, 1, &disable, sizeof(disable));
    }
    if (status == noErr) {
        status = AudioUnitSetProperty(state.outputUnit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0, &output.id, sizeof(output.id));
    }
    if (status == noErr) {
        status = AudioUnitSetProperty(state.outputUnit, kAudioUnitProperty_StreamFormat,
                                      kAudioUnitScope_Input, 0, &format, sizeof(format));
    }
    if (status == noErr) {
        SInt32 map[] = {static_cast<SInt32>(output.channel - 1)};
        status = AudioUnitSetProperty(state.outputUnit, kAudioOutputUnitProperty_ChannelMap,
                                      kAudioUnitScope_Input, 0, map, sizeof(map));
    }
    if (status == noErr) {
        AURenderCallbackStruct callback {outputCallback, &state};
        status = AudioUnitSetProperty(state.outputUnit, kAudioUnitProperty_SetRenderCallback,
                                      kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    }
    if (status != noErr) {
        fail("Unable to configure output AUHAL", status);
    }
    status = AudioUnitInitialize(state.outputUnit);
    if (status != noErr) {
        fail("Unable to initialize output AUHAL", status);
    }
}

DeviceSelection selectDevice(const char* uid, UInt32 channel, AudioObjectPropertyScope scope) {
    DeviceSelection selection;
    selection.id = rsbridge::deviceForUIDString(uid);
    if (selection.id == kAudioObjectUnknown) {
        std::fprintf(stderr, "Device UID not found: %s\n", uid);
        std::exit(2);
    }
    UInt32 channels = rsbridge::channelCount(selection.id, scope);
    if (channel == 0 || channel > channels) {
        std::fprintf(stderr, "%s has %u %s channels; channel %u is invalid.\n",
                     uid, channels, scope == kAudioObjectPropertyScopeInput ? "input" : "output", channel);
        std::exit(64);
    }
    selection.name = rsbridge::copyStringProperty(selection.id, kAudioObjectPropertyName);
    selection.uid = uid;
    selection.channel = channel;
    return selection;
}

void cleanup(MeasureState& state) {
    if (state.outputUnit != nullptr) {
        AudioOutputUnitStop(state.outputUnit);
        AudioUnitUninitialize(state.outputUnit);
        AudioComponentInstanceDispose(state.outputUnit);
    }
    if (state.inputUnit != nullptr) {
        AudioOutputUnitStop(state.inputUnit);
        AudioUnitUninitialize(state.inputUnit);
        AudioComponentInstanceDispose(state.inputUnit);
    }
    if (state.inputBuffer != nullptr) {
        std::free(state.inputBuffer->mBuffers[0].mData);
        std::free(state.inputBuffer);
    }
}

void listDevices() {
    for (AudioObjectID device : rsbridge::allDevices()) {
        UInt32 inputs = rsbridge::channelCount(device, kAudioObjectPropertyScopeInput);
        UInt32 outputs = rsbridge::channelCount(device, kAudioObjectPropertyScopeOutput);
        if (inputs == 0 && outputs == 0) {
            continue;
        }
        std::printf("%u\tin:%u\tout:%u\trate:%.0f\t%s\t%s\n",
                    device,
                    inputs,
                    outputs,
                    rsbridge::nominalSampleRate(device),
                    rsbridge::copyStringProperty(device, kAudioObjectPropertyName).c_str(),
                    rsbridge::copyStringProperty(device, kAudioDevicePropertyDeviceUID).c_str());
    }
}

void measure(const char* outputUID, UInt32 outputChannel, const char* inputUID, UInt32 inputChannel,
             UInt32 timeoutMs, float threshold, float amplitude) {
    DeviceSelection output = selectDevice(outputUID, outputChannel, kAudioObjectPropertyScopeOutput);
    DeviceSelection input = selectDevice(inputUID, inputChannel, kAudioObjectPropertyScopeInput);

    MeasureState state;
    state.ticksPerFrame = (1000000000.0 / kMeasureSampleRate) / nanosPerHostTick();
    state.warmupFrames = static_cast<UInt64>(kMeasureSampleRate / 2);
    state.detectThreshold = threshold;
    state.pulseAmplitude = amplitude;
    state.patternEnergy = pulsePatternEnergy(amplitude);
    configureInput(state, input);
    configureOutput(state, output);

    OSStatus status = AudioOutputUnitStart(state.inputUnit);
    if (status != noErr) {
        fail("Unable to start input", status);
    }
    status = AudioOutputUnitStart(state.outputUnit);
    if (status != noErr) {
        fail("Unable to start output", status);
    }

    std::printf("output: %s channel %u\n", output.name.c_str(), output.channel);
    std::printf("input: %s channel %u\n", input.name.c_str(), input.channel);
    std::printf("pulse: amplitude %.2f, frames %u, correlation-threshold %.3f\n",
                state.pulseAmplitude, kPulseFrames, state.detectThreshold);

    UInt32 waitedMs = 0;
    while (!state.detected.load(std::memory_order_acquire) && waitedMs < timeoutMs &&
           state.error.load(std::memory_order_relaxed) == noErr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitedMs += 10;
    }

    OSStatus renderError = state.error.load(std::memory_order_relaxed);
    if (renderError != noErr) {
        cleanup(state);
        fail("Render error during measurement", renderError);
    }
    if (!state.detected.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "observed-peak: %.4f\n", state.observedPeak.load(std::memory_order_acquire));
        std::fprintf(stderr, "input-callbacks: %llu\n",
                     static_cast<unsigned long long>(state.inputCallbacks.load(std::memory_order_acquire)));
        std::fprintf(stderr, "output-callbacks: %llu\n",
                     static_cast<unsigned long long>(state.outputCallbacks.load(std::memory_order_acquire)));
        std::fprintf(stderr, "pulse-sent: %s\n",
                     state.pulseSent.load(std::memory_order_acquire) ? "yes" : "no");
        cleanup(state);
        fail("No loopback pulse detected. Check cable, output level, input gain, and selected channels.");
    }

    auto pulseHost = static_cast<int64_t>(state.pulseHostTime.load(std::memory_order_acquire));
    auto detectedHost = static_cast<int64_t>(state.detectedHostTime.load(std::memory_order_acquire));
    int64_t deltaHost = detectedHost - pulseHost;
    if (deltaHost < 0) {
        std::fprintf(stderr, "Detected signal before output timestamp; measurement is not trustworthy.\n");
        std::fprintf(stderr, "signed-host-delta-ticks: %lld\n", static_cast<long long>(deltaHost));
        std::fprintf(stderr, "detected-peak: %.4f\n", state.detectedPeak.load(std::memory_order_acquire));
        cleanup(state);
        std::exit(1);
    }

    double latencyMs = static_cast<double>(deltaHost) *
                       nanosPerHostTick() / 1000000.0;
    double latencyFrames = latencyMs * kMeasureSampleRate / 1000.0;
    std::printf("round-trip-latency-ms: %.3f\n", latencyMs);
    std::printf("round-trip-latency-frames-at-48000: %.1f\n", latencyFrames);
    std::printf("detected-peak: %.4f\n", state.detectedPeak.load(std::memory_order_acquire));
    cleanup(state);
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s --list\n"
                 "  %s OUTPUT_UID OUTPUT_CHANNEL INPUT_UID INPUT_CHANNEL [TIMEOUT_MS] [CORRELATION_THRESHOLD] [AMPLITUDE]\n",
                 argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        listDevices();
        return 0;
    }
    if (argc < 5 || argc > 8) {
        usage(argv[0]);
        return 64;
    }
    UInt32 outputChannel = parseUInt(argv[2], 1, 1024, "output channel");
    UInt32 inputChannel = parseUInt(argv[4], 1, 1024, "input channel");
    UInt32 timeoutMs = argc >= 6 ? parseUInt(argv[5], 1000, 30000, "timeout ms") : 5000;
    float threshold = argc >= 7 ? parseFloat(argv[6], 0.05f, 1.0f, "correlation threshold") : kDefaultDetectThreshold;
    float amplitude = argc >= 8 ? parseFloat(argv[7], 0.01f, 1.0f, "amplitude") : kDefaultPulseAmplitude;
    measure(argv[1], outputChannel, argv[3], inputChannel, timeoutMs, threshold, amplitude);
    return 0;
}
