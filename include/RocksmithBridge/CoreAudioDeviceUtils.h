#pragma once

#include "RocksmithBridge/Config.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace rsbridge {

struct InputDeviceInfo {
    AudioObjectID id = kAudioObjectUnknown;
    std::string name;
    std::string manufacturer;
    std::string uid;
    uint32_t inputChannels = 0;
    double nominalSampleRate = 0.0;
};

inline std::string copyStringProperty(AudioObjectID objectID, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    OSStatus status = AudioObjectGetPropertyData(objectID, &address, 0, nullptr, &size, &value);
    if (status != noErr || value == nullptr) {
        return {};
    }
    std::string result = cfStringToStdString(value);
    CFRelease(value);
    return result;
}

inline uint32_t channelCount(AudioObjectID deviceID, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(deviceID, &address, 0, nullptr, &size);
    if (status != noErr || size == 0) {
        return 0;
    }
    auto* list = static_cast<AudioBufferList*>(std::calloc(1, size));
    if (list == nullptr) {
        return 0;
    }
    status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, list);
    uint32_t channels = 0;
    if (status == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
            channels += list->mBuffers[i].mNumberChannels;
        }
    }
    std::free(list);
    return channels;
}

inline double nominalSampleRate(AudioObjectID deviceID) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyNominalSampleRate,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    Float64 sampleRate = 0;
    UInt32 size = sizeof(sampleRate);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &sampleRate);
    return status == noErr ? sampleRate : 0.0;
}

inline AudioObjectID deviceForUID(CFStringRef uid) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectID deviceID = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceID);
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                 &address,
                                                 sizeof(uid),
                                                 &uid,
                                                 &size,
                                                 &deviceID);
    return status == noErr ? deviceID : kAudioObjectUnknown;
}

inline std::vector<AudioObjectID> allDevices() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr || size == 0) {
        return {};
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) {
        return {};
    }
    return devices;
}

inline AudioObjectID deviceForUIDString(const std::string& uid) {
    if (uid.empty()) {
        return kAudioObjectUnknown;
    }
    CFStringRef cfUID = createCFString(uid);
    AudioObjectID deviceID = deviceForUID(cfUID);
    CFRelease(cfUID);
    if (deviceID != kAudioObjectUnknown) {
        return deviceID;
    }
    for (AudioObjectID device : allDevices()) {
        if (copyStringProperty(device, kAudioDevicePropertyDeviceUID) == uid) {
            return device;
        }
    }
    return kAudioObjectUnknown;
}

inline std::vector<InputDeviceInfo> inputDevices() {
    std::vector<InputDeviceInfo> result;
    for (AudioObjectID device : allDevices()) {
        uint32_t inputs = channelCount(device, kAudioObjectPropertyScopeInput);
        if (inputs == 0) {
            continue;
        }
        InputDeviceInfo info;
        info.id = device;
        info.name = copyStringProperty(device, kAudioObjectPropertyName);
        info.manufacturer = copyStringProperty(device, kAudioObjectPropertyManufacturer);
        info.uid = copyStringProperty(device, kAudioDevicePropertyDeviceUID);
        info.inputChannels = inputs;
        info.nominalSampleRate = nominalSampleRate(device);
        result.push_back(info);
    }
    return result;
}

inline bool looksLikeMotuM4(const InputDeviceInfo& info) {
    std::string haystack = info.name + " " + info.manufacturer;
    return info.inputChannels > 0 && haystack.find("MOTU") != std::string::npos && haystack.find("M4") != std::string::npos;
}

inline InputDeviceInfo infoForDevice(AudioObjectID deviceID) {
    InputDeviceInfo info;
    if (deviceID == kAudioObjectUnknown) {
        return info;
    }
    info.id = deviceID;
    info.name = copyStringProperty(deviceID, kAudioObjectPropertyName);
    info.manufacturer = copyStringProperty(deviceID, kAudioObjectPropertyManufacturer);
    info.uid = copyStringProperty(deviceID, kAudioDevicePropertyDeviceUID);
    info.inputChannels = channelCount(deviceID, kAudioObjectPropertyScopeInput);
    info.nominalSampleRate = nominalSampleRate(deviceID);
    return info;
}

inline bool hasInputChannelRange(const InputDeviceInfo& info, uint32_t firstChannel,
                                 uint32_t channelCount = kBridgePlayerCount) {
    return firstChannel > 0 && channelCount > 0 && info.inputChannels >= firstChannel &&
           info.inputChannels - firstChannel + 1 >= channelCount;
}

} // namespace rsbridge
