#include "RocksmithBridge/SharedRingBuffer.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <mach/mach_time.h>
#include <os/log.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr UInt32 kPlayerCount = rsbridge::kBridgePlayerCount;
constexpr AudioObjectID kDeviceObjectIDs[kPlayerCount] = {2, 4};
constexpr AudioObjectID kStreamObjectIDs[kPlayerCount] = {3, 5};
const CFStringRef kBundleID = CFSTR("com.vhusso.rocksmithbridge.driver");
const CFStringRef kDeviceUID = CFSTR("com.vhusso.rocksmithbridge.device");
const CFStringRef kDevice2UID = CFSTR("com.vhusso.rocksmithbridge.device.2");
const CFStringRef kModelUID = CFSTR("com.vhusso.rocksmithbridge.model");
const CFStringRef kModel2UID = CFSTR("com.vhusso.rocksmithbridge.model.2");
const CFStringRef kDeviceName = CFSTR("Rocksmith MOTU Bridge Source 1");
const CFStringRef kDevice2Name = CFSTR("Rocksmith MOTU Bridge Source 2");
const CFStringRef kManufacturer = CFSTR("Rocksmith Bridge");
constexpr UInt32 kZeroTimestampPeriod = 12000;

std::atomic<UInt32> gRefCount{1};
std::atomic<UInt32> gRunningClients[kPlayerCount];
AudioServerPlugInHostRef gHost = nullptr;
rsbridge::SharedRing gRings[kPlayerCount];
std::atomic<uint64_t> gReadFrame[kPlayerCount];
std::atomic<uint64_t> gAnchorHostTime[kPlayerCount];
std::atomic<Float64> gHostTicksPerFrame{0};
std::atomic<UInt64> gTimestampSeed[kPlayerCount];
std::atomic<UInt32> gBufferFrameSize[kPlayerCount];
std::atomic<bool> gUseFloatFormat[kPlayerCount];

AudioStreamBasicDescription gFloatFormat = {static_cast<Float64>(rsbridge::kSampleRate), kAudioFormatLinearPCM,
                                            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian,
                                            sizeof(float), 1, sizeof(float), rsbridge::kChannelCount, 32, 0};

AudioStreamBasicDescription gInt16Format = {static_cast<Float64>(rsbridge::kSampleRate), kAudioFormatLinearPCM,
                                            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian,
                                            sizeof(int16_t), 1, sizeof(int16_t), rsbridge::kChannelCount, 16, 0};

ULONG STDMETHODCALLTYPE AddRef(void*);

AudioStreamBasicDescription currentFormat(int player) { return gUseFloatFormat[player].load(std::memory_order_acquire) ? gFloatFormat : gInt16Format; }

void refreshRing(UInt32 player) {
    if (player >= kPlayerCount || gRings[player].valid() || gRunningClients[player].load(std::memory_order_acquire) > 0) { return; }
    rsbridge::openSharedRingForPlayer(gRings[player], player + 1, false, nullptr, rsbridge::SharedRingAccess::readOnly);
}

void notifyPropertyChanged(AudioObjectID objectID, const AudioObjectPropertyAddress& address) {
    if (gHost != nullptr && gHost->PropertiesChanged != nullptr) {
        gHost->PropertiesChanged(gHost, objectID, 1, &address);
    }
}

CFUUIDRef factoryUUID() {
    return CFUUIDGetConstantUUIDWithBytes(nullptr,
                                          0x0F, 0x3C, 0xCB, 0x9A, 0x3C, 0x46, 0x4E, 0x7A,
                                          0x9B, 0x40, 0x7E, 0x68, 0x78, 0xEC, 0x03, 0xEF);
}

bool isInputScope(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeInput || scope == kAudioObjectPropertyScopeGlobal;
}

bool isOutputScope(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeOutput;
}

bool sameFormat(const AudioStreamBasicDescription& a, const AudioStreamBasicDescription& b) {
    return a.mSampleRate == b.mSampleRate &&
           a.mFormatID == b.mFormatID &&
           a.mFormatFlags == b.mFormatFlags &&
           a.mBytesPerPacket == b.mBytesPerPacket &&
           a.mFramesPerPacket == b.mFramesPerPacket &&
           a.mBytesPerFrame == b.mBytesPerFrame &&
           a.mChannelsPerFrame == b.mChannelsPerFrame &&
           a.mBitsPerChannel == b.mBitsPerChannel;
}

AudioClassID classForObject(AudioObjectID objectID) {
    if (objectID == kAudioObjectPlugInObject) {
        return kAudioPlugInClassID;
    }
    for (UInt32 i = 0; i < kPlayerCount; ++i) {
        if (objectID == kDeviceObjectIDs[i]) {
            return kAudioDeviceClassID;
        }
        if (objectID == kStreamObjectIDs[i]) {
            return kAudioStreamClassID;
        }
    }
    return kAudioObjectClassID;
}

AudioObjectID ownerForObject(AudioObjectID objectID) {
    if (objectID == kAudioObjectPlugInObject) {
        return kAudioObjectUnknown;
    }
    for (UInt32 i = 0; i < kPlayerCount; ++i) {
        if (objectID == kDeviceObjectIDs[i]) {
            return kAudioObjectPlugInObject;
        }
        if (objectID == kStreamObjectIDs[i]) {
            return kDeviceObjectIDs[i];
        }
    }
    return kAudioObjectUnknown;
}

int playerForDevice(AudioObjectID objectID) {
    for (UInt32 i = 0; i < kPlayerCount; ++i) {
        if (objectID == kDeviceObjectIDs[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int playerForStream(AudioObjectID objectID) {
    for (UInt32 i = 0; i < kPlayerCount; ++i) {
        if (objectID == kStreamObjectIDs[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int playerForObject(AudioObjectID objectID) {
    int player = playerForDevice(objectID);
    return player >= 0 ? player : playerForStream(objectID);
}

CFStringRef deviceUID(int player) {
    return player == 0 ? kDeviceUID : kDevice2UID;
}

CFStringRef modelUID(int player) {
    return player == 0 ? kModelUID : kModel2UID;
}

CFStringRef deviceName(int player) {
    return player == 0 ? kDeviceName : kDevice2Name;
}

template <typename T>
OSStatus writeScalar(UInt32 inDataSize, UInt32* outDataSize, void* outData, const T& value) {
    if (inDataSize < sizeof(T)) {
        return kAudioHardwareBadPropertySizeError;
    }
    *static_cast<T*>(outData) = value;
    *outDataSize = sizeof(T);
    return kAudioHardwareNoError;
}

OSStatus writeCFString(UInt32 inDataSize, UInt32* outDataSize, void* outData, CFStringRef value) {
    if (inDataSize < sizeof(CFStringRef)) {
        return kAudioHardwareBadPropertySizeError;
    }
    *static_cast<CFStringRef*>(outData) = static_cast<CFStringRef>(CFRetain(value));
    *outDataSize = sizeof(CFStringRef);
    return kAudioHardwareNoError;
}

UInt32 streamConfigurationSize(bool hasInput) {
    return hasInput ? sizeof(AudioBufferList) : offsetof(AudioBufferList, mBuffers);
}

OSStatus writeStreamConfiguration(UInt32 inDataSize, UInt32* outDataSize, void* outData, bool hasInput) {
    const UInt32 size = streamConfigurationSize(hasInput);
    if (inDataSize < size) {
        return kAudioHardwareBadPropertySizeError;
    }
    auto* list = static_cast<AudioBufferList*>(outData);
    list->mNumberBuffers = hasInput ? 1 : 0;
    if (hasInput) {
        list->mBuffers[0].mNumberChannels = rsbridge::kChannelCount;
        list->mBuffers[0].mDataByteSize = 0;
        list->mBuffers[0].mData = nullptr;
    }
    *outDataSize = size;
    return kAudioHardwareNoError;
}

AudioStreamRangedDescription rangedDescription(const AudioStreamBasicDescription& format) {
    return {format, {format.mSampleRate, format.mSampleRate}};
}

bool hasObject(AudioObjectID objectID) {
    return objectID == kAudioObjectPlugInObject || playerForObject(objectID) >= 0;
}

bool pluginHasProperty(AudioObjectPropertySelector selector) {
    switch (selector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyClockDeviceList:
        case kAudioPlugInPropertyTranslateUIDToClockDevice:
            return true;
        default:
            return false;
    }
}

bool deviceHasProperty(AudioObjectPropertySelector selector) {
    switch (selector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyBufferFrameSizeRange:
        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
        case kAudioDevicePropertyStreamConfiguration:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
            return true;
        default:
            return false;
    }
}

bool streamHasProperty(AudioObjectPropertySelector selector) {
    switch (selector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        default:
            return false;
    }
}

Boolean STDMETHODCALLTYPE HasProperty(AudioServerPlugInDriverRef, AudioObjectID objectID, pid_t,
                                      const AudioObjectPropertyAddress* address) {
    if (address == nullptr || !hasObject(objectID)) {
        return false;
    }
    if (objectID == kAudioObjectPlugInObject) {
        return pluginHasProperty(address->mSelector);
    }
    if (playerForDevice(objectID) >= 0) {
        return deviceHasProperty(address->mSelector);
    }
    return streamHasProperty(address->mSelector);
}

OSStatus STDMETHODCALLTYPE IsPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t pid,
                                             const AudioObjectPropertyAddress* address, Boolean* outIsSettable) {
    if (outIsSettable == nullptr || !HasProperty(driver, objectID, pid, address)) {
        return kAudioHardwareUnknownPropertyError;
    }
    *outIsSettable = (playerForDevice(objectID) >= 0 && address->mSelector == kAudioDevicePropertyBufferFrameSize) ||
                     (playerForStream(objectID) >= 0 &&
                      (address->mSelector == kAudioStreamPropertyVirtualFormat ||
                       address->mSelector == kAudioStreamPropertyPhysicalFormat));
    return kAudioHardwareNoError;
}

OSStatus propertyDataSize(AudioObjectID objectID, const AudioObjectPropertyAddress* address,
                          UInt32 qualifierSize, const void* qualifierData, UInt32* outDataSize) {
    if (outDataSize == nullptr || address == nullptr) {
        return kAudioHardwareBadPropertySizeError;
    }
    if (!hasObject(objectID)) {
        return kAudioHardwareBadObjectError;
    }
    if (!HasProperty(nullptr, objectID, 0, address)) {
        return kAudioHardwareUnknownPropertyError;
    }
    switch (address->mSelector) {
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyResourceBundle:
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyBufferFrameSizeRange:
            *outDataSize = sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return kAudioHardwareNoError;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = sizeof(AudioStreamRangedDescription) * 2;
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = objectID == kAudioObjectPlugInObject ? sizeof(AudioObjectID) * kPlayerCount
                           : playerForDevice(objectID) >= 0 ? sizeof(AudioObjectID) : 0;
            if (address->mSelector == kAudioObjectPropertyCustomPropertyInfoList) {
                *outDataSize = 0;
            }
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyDeviceList:
            *outDataSize = sizeof(AudioObjectID) * kPlayerCount;
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList:
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyTranslateUIDToClockDevice:
            *outDataSize = sizeof(AudioObjectID);
            return (qualifierSize == sizeof(CFStringRef) && qualifierData != nullptr) ? kAudioHardwareNoError
                                                                                      : kAudioHardwareBadPropertySizeError;
        case kAudioDevicePropertyStreams:
            *outDataSize = isInputScope(address->mScope) && !isOutputScope(address->mScope) ? sizeof(AudioObjectID) : 0;
            return kAudioHardwareNoError;
        case kAudioDevicePropertyStreamConfiguration:
            *outDataSize = streamConfigurationSize(isInputScope(address->mScope) && !isOutputScope(address->mScope));
            return kAudioHardwareNoError;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus STDMETHODCALLTYPE GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID objectID, pid_t,
                                              const AudioObjectPropertyAddress* address, UInt32 qualifierSize,
                                              const void* qualifierData, UInt32* outDataSize) {
    return propertyDataSize(objectID, address, qualifierSize, qualifierData, outDataSize);
}

OSStatus STDMETHODCALLTYPE GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID objectID, pid_t,
                                          const AudioObjectPropertyAddress* address, UInt32 qualifierSize,
                                          const void* qualifierData, UInt32 inDataSize, UInt32* outDataSize,
                                          void* outData) {
    if (address == nullptr || outDataSize == nullptr || outData == nullptr) {
        return kAudioHardwareBadPropertySizeError;
    }
    if (!hasObject(objectID)) {
        return kAudioHardwareBadObjectError;
    }
    if (!HasProperty(nullptr, objectID, 0, address)) {
        return kAudioHardwareUnknownPropertyError;
    }

    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
            return writeScalar(inDataSize, outDataSize, outData, classForObject(objectID));
        case kAudioObjectPropertyClass:
            return writeScalar(inDataSize, outDataSize, outData, classForObject(objectID));
        case kAudioObjectPropertyOwner:
            return writeScalar(inDataSize, outDataSize, outData, ownerForObject(objectID));
        case kAudioObjectPropertyName:
            if (objectID == kAudioObjectPlugInObject) {
                return writeCFString(inDataSize, outDataSize, outData, CFSTR("Rocksmith MOTU Bridge"));
            }
            return writeCFString(inDataSize, outDataSize, outData,
                                 playerForStream(objectID) >= 0 ? CFSTR("Guitar Input") : deviceName(playerForObject(objectID)));
        case kAudioObjectPropertyModelName:
            if (objectID == kAudioObjectPlugInObject) {
                return writeCFString(inDataSize, outDataSize, outData, CFSTR("Rocksmith MOTU Bridge"));
            }
            return writeCFString(inDataSize, outDataSize, outData, deviceName(playerForObject(objectID)));
        case kAudioObjectPropertyManufacturer:
            return writeCFString(inDataSize, outDataSize, outData, kManufacturer);
        case kAudioPlugInPropertyBundleID:
            return writeCFString(inDataSize, outDataSize, outData, kBundleID);
        case kAudioPlugInPropertyResourceBundle:
            return writeCFString(inDataSize, outDataSize, outData, CFSTR(""));
        case kAudioPlugInPropertyDeviceList:
        case kAudioObjectPropertyOwnedObjects: {
            UInt32 size = 0;
            auto status = propertyDataSize(objectID, address, qualifierSize, qualifierData, &size);
            if (status != kAudioHardwareNoError || inDataSize < size) {
                return status == kAudioHardwareNoError ? kAudioHardwareBadPropertySizeError : status;
            }
            auto* objects = static_cast<AudioObjectID*>(outData);
            if (objectID == kAudioObjectPlugInObject) {
                for (UInt32 i = 0; i < kPlayerCount; ++i) {
                    objects[i] = kDeviceObjectIDs[i];
                }
            } else if (playerForDevice(objectID) >= 0) {
                objects[0] = kStreamObjectIDs[playerForDevice(objectID)];
            }
            *outDataSize = size;
            return kAudioHardwareNoError;
        }
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList:
        case kAudioObjectPropertyControlList:
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (qualifierSize != sizeof(CFStringRef) || qualifierData == nullptr) {
                return kAudioHardwareBadPropertySizeError;
            }
            CFStringRef uid = *static_cast<CFStringRef const*>(qualifierData);
            AudioObjectID device = CFEqual(uid, kDeviceUID) ? kDeviceObjectIDs[0]
                                 : CFEqual(uid, kDevice2UID) ? kDeviceObjectIDs[1]
                                 : kAudioObjectUnknown;
            return writeScalar(inDataSize, outDataSize, outData, device);
        }
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyTranslateUIDToClockDevice:
            if (qualifierSize != sizeof(CFStringRef) || qualifierData == nullptr) {
                return kAudioHardwareBadPropertySizeError;
            }
            return writeScalar(inDataSize, outDataSize, outData, static_cast<AudioObjectID>(kAudioObjectUnknown));
        case kAudioDevicePropertyDeviceUID:
            return writeCFString(inDataSize, outDataSize, outData, deviceUID(playerForDevice(objectID)));
        case kAudioDevicePropertyModelUID:
            return writeCFString(inDataSize, outDataSize, outData, modelUID(playerForDevice(objectID)));
        case kAudioDevicePropertyTransportType:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(kAudioDeviceTransportTypeVirtual));
        case kAudioDevicePropertyClockDomain:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(0));
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyClockIsStable:
        case kAudioStreamPropertyIsActive:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(1));
        case kAudioDevicePropertyDeviceIsRunning:
            return writeScalar(inDataSize, outDataSize, outData,
                               static_cast<UInt32>(gRunningClients[playerForDevice(objectID)].load() > 0 ? 1 : 0));
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(address->mScope == kAudioObjectPropertyScopeInput ? 1 : 0));
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(0));
        case kAudioDevicePropertyBufferFrameSize:
            return writeScalar(inDataSize, outDataSize, outData,
                               gBufferFrameSize[playerForDevice(objectID)].load(std::memory_order_acquire));
        case kAudioDevicePropertyZeroTimeStampPeriod:
            return writeScalar(inDataSize, outDataSize, outData, kZeroTimestampPeriod);
        case kAudioDevicePropertyClockAlgorithm:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(kAudioDeviceClockAlgorithmRaw));
        case kAudioDevicePropertyNominalSampleRate:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<Float64>(rsbridge::kSampleRate));
        case kAudioDevicePropertyAvailableNominalSampleRates:
            return writeScalar(inDataSize, outDataSize, outData, AudioValueRange{rsbridge::kSampleRate, rsbridge::kSampleRate});
        case kAudioDevicePropertyBufferFrameSizeRange:
            return writeScalar(inDataSize, outDataSize, outData, AudioValueRange{rsbridge::kMinBufferFrames, 2048});
        case kAudioDevicePropertyStreams: {
            UInt32 size = isInputScope(address->mScope) && !isOutputScope(address->mScope) ? sizeof(AudioObjectID) : 0;
            if (inDataSize < size) {
                return kAudioHardwareBadPropertySizeError;
            }
            if (size == sizeof(AudioObjectID)) {
                *static_cast<AudioObjectID*>(outData) = kStreamObjectIDs[playerForDevice(objectID)];
            }
            *outDataSize = size;
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyStreamConfiguration:
            return writeStreamConfiguration(inDataSize, outDataSize, outData,
                                            isInputScope(address->mScope) && !isOutputScope(address->mScope));
        case kAudioStreamPropertyDirection:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(1));
        case kAudioStreamPropertyTerminalType:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(kAudioStreamTerminalTypeLine));
        case kAudioStreamPropertyStartingChannel:
            return writeScalar(inDataSize, outDataSize, outData, static_cast<UInt32>(1));
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            return writeScalar(inDataSize, outDataSize, outData, currentFormat(playerForStream(objectID)));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            if (inDataSize < sizeof(AudioStreamRangedDescription) * 2) {
                return kAudioHardwareBadPropertySizeError;
            }
            auto* formats = static_cast<AudioStreamRangedDescription*>(outData);
            formats[0] = rangedDescription(gFloatFormat);
            formats[1] = rangedDescription(gInt16Format);
            *outDataSize = sizeof(AudioStreamRangedDescription) * 2;
            return kAudioHardwareNoError;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus STDMETHODCALLTYPE SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID objectID, pid_t,
                                          const AudioObjectPropertyAddress* address, UInt32, const void*,
                                          UInt32 inDataSize, const void* inData) {
    if (address == nullptr || inData == nullptr) {
        return kAudioHardwareBadPropertySizeError;
    }
    int devicePlayer = playerForDevice(objectID);
    int streamPlayer = playerForStream(objectID);
    if (devicePlayer >= 0 && address->mSelector == kAudioDevicePropertyBufferFrameSize) {
        if (inDataSize != sizeof(UInt32)) {
            return kAudioHardwareBadPropertySizeError;
        }
        UInt32 requested = *static_cast<const UInt32*>(inData);
        if (requested < rsbridge::kMinBufferFrames || requested > 2048) {
            return kAudioHardwareIllegalOperationError;
        }
        gBufferFrameSize[devicePlayer].store(requested, std::memory_order_release);
        notifyPropertyChanged(objectID, *address);
        return kAudioHardwareNoError;
    }
    if (streamPlayer >= 0 &&
        (address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inDataSize != sizeof(AudioStreamBasicDescription)) {
            return kAudioHardwareBadPropertySizeError;
        }
        auto requested = *static_cast<const AudioStreamBasicDescription*>(inData);
        if (!sameFormat(requested, gFloatFormat) && !sameFormat(requested, gInt16Format)) {
            return kAudioDeviceUnsupportedFormatError;
        }
        gUseFloatFormat[streamPlayer].store(sameFormat(requested, gFloatFormat), std::memory_order_release);
        notifyPropertyChanged(objectID, *address);
        return kAudioHardwareNoError;
    }
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
    os_log(OS_LOG_DEFAULT, "RocksmithMotuBridge Initialize");
    gHost = host;
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const double nanosPerTick = static_cast<double>(timebase.numer) / static_cast<double>(timebase.denom);
    gHostTicksPerFrame.store((1000000000.0 / static_cast<double>(rsbridge::kSampleRate)) / nanosPerTick,
                             std::memory_order_release);
    for (UInt32 i = 0; i < kPlayerCount; ++i) {
        gRunningClients[i].store(0, std::memory_order_release);
        gReadFrame[i].store(0, std::memory_order_release);
        gAnchorHostTime[i].store(mach_absolute_time(), std::memory_order_release);
        gTimestampSeed[i].store(1, std::memory_order_release);
        gBufferFrameSize[i].store(rsbridge::kDefaultBufferFrames, std::memory_order_release);
        gUseFloatFormat[i].store(true, std::memory_order_release);
        refreshRing(i);
    }
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE StartIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32) {
    int player = playerForDevice(deviceID);
    if (player < 0) {
        return kAudioHardwareBadDeviceError;
    }
    const UInt32 previous = gRunningClients[player].fetch_add(1);
    if (previous == 0) {
        uint64_t readFrame = 0;
        if (gRings[player].valid()) {
            const uint64_t writeFrame = gRings[player].header->writeFrame.load(std::memory_order_acquire);
            const uint32_t latency = rsbridge::clampTargetLatencyFrames(gRings[player].header->targetLatencyFrames.load(std::memory_order_acquire));
            readFrame = writeFrame > latency ? writeFrame - latency : 0;
        }
        gReadFrame[player].store(readFrame, std::memory_order_release);
        gAnchorHostTime[player].store(mach_absolute_time(), std::memory_order_release);
        gTimestampSeed[player].fetch_add(1, std::memory_order_acq_rel);
    }
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE StopIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32) {
    int player = playerForDevice(deviceID);
    if (player < 0) {
        return kAudioHardwareBadDeviceError;
    }
    UInt32 current = gRunningClients[player].load();
    while (current > 0 && !gRunningClients[player].compare_exchange_weak(current, current - 1)) {}
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32,
                                           Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    int player = playerForDevice(deviceID);
    if (player < 0 || outSampleTime == nullptr || outHostTime == nullptr || outSeed == nullptr) {
        return kAudioHardwareBadDeviceError;
    }
    const uint64_t now = mach_absolute_time();
    const auto anchorHostTime = gAnchorHostTime[player].load(std::memory_order_acquire);
    const auto hostTicksPerFrame = gHostTicksPerFrame.load(std::memory_order_acquire);
    const auto elapsedFrames = hostTicksPerFrame > 0
                                   ? static_cast<uint64_t>((now - anchorHostTime) / hostTicksPerFrame)
                                   : static_cast<uint64_t>(0);
    const auto periods = elapsedFrames / kZeroTimestampPeriod;
    *outSampleTime = static_cast<Float64>(periods * kZeroTimestampPeriod);
    *outHostTime = anchorHostTime + static_cast<uint64_t>(*outSampleTime * hostTicksPerFrame);
    *outSeed = gTimestampSeed[player].load(std::memory_order_acquire);
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32, UInt32 operationID,
                                            Boolean* outWillDo, Boolean* outWillDoInPlace) {
    if (playerForDevice(deviceID) < 0 || outWillDo == nullptr || outWillDoInPlace == nullptr) {
        return kAudioHardwareBadDeviceError;
    }
    *outWillDo = operationID == kAudioServerPlugInIOOperationReadInput;
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID deviceID, AudioObjectID streamID,
                                        UInt32, UInt32 operationID, UInt32 frameCount,
                                        const AudioServerPlugInIOCycleInfo*, void* ioMainBuffer, void*) {
    int player = playerForDevice(deviceID);
    if (player < 0 || streamID != kStreamObjectIDs[player]) {
        return kAudioHardwareBadObjectError;
    }
    if (operationID != kAudioServerPlugInIOOperationReadInput || ioMainBuffer == nullptr) {
        return kAudioHardwareNoError;
    }

    float scratch[4096];
    UInt32 remaining = frameCount;
    UInt32 offset = 0;
    uint64_t readFrame = gReadFrame[player].load(std::memory_order_acquire);
    while (remaining > 0) {
        UInt32 chunk = remaining > 4096 ? 4096 : remaining;
        rsbridge::readMonoFrames(gRings[player], readFrame, scratch, chunk);
        if (gUseFloatFormat[player].load(std::memory_order_acquire)) {
            std::memcpy(static_cast<float*>(ioMainBuffer) + offset, scratch, sizeof(float) * chunk);
        } else {
            auto* out = static_cast<int16_t*>(ioMainBuffer) + offset;
            for (UInt32 i = 0; i < chunk; ++i) {
                float clipped = std::fmax(-1.0f, std::fmin(1.0f, scratch[i]));
                out[i] = static_cast<int16_t>(clipped * 32767.0f);
            }
        }
        offset += chunk;
        remaining -= chunk;
    }
    gReadFrame[player].store(readFrame, std::memory_order_release);
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE NoOpConfig(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) {
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE AddRemoveClient(AudioServerPlugInDriverRef, AudioObjectID deviceID, const AudioServerPlugInClientInfo*) {
    int player = playerForDevice(deviceID);
    if (player < 0) {
        return kAudioHardwareBadDeviceError;
    }
    refreshRing(static_cast<UInt32>(player));
    return kAudioHardwareNoError;
}

OSStatus STDMETHODCALLTYPE BeginEndIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32, UInt32, UInt32,
                                     const AudioServerPlugInIOCycleInfo*) {
    return playerForDevice(deviceID) >= 0 ? kAudioHardwareNoError : kAudioHardwareBadDeviceError;
}

#include "RocksmithBridgeDriverPlugin.inc"

} // namespace

extern "C" __attribute__((visibility("default"))) void* RocksmithBridge_Create(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        AddRef(nullptr);
        CFPlugInAddInstanceForFactory(factoryUUID());
        return &gDriverInterfacePtr;
    }
    return nullptr;
}
