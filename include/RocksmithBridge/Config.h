#pragma once

#include "RocksmithBridge/SharedRingBuffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>

namespace rsbridge {

inline constexpr char kAppSupportDir[] = "Library/Application Support/RocksmithMotuBridge";
inline constexpr char kConfigFileName[] = "config.plist";
inline constexpr char kVirtualDeviceUID[] = "com.vhusso.rocksmithbridge.device";
inline constexpr char kVirtualDevice2UID[] = "com.vhusso.rocksmithbridge.device.2";
inline constexpr char kAggregateDeviceUID[] = "com.vhusso.rocksmithbridge.aggregate";
inline constexpr char kAggregateDevice2UID[] = "com.vhusso.rocksmithbridge.aggregate.2";
inline constexpr char kAggregateDeviceName[] = "Rocksmith USB Guitar Adapter";
inline constexpr char kAggregateDevice1Name[] = "Rocksmith USB Guitar Adapter 1";
inline constexpr char kAggregateDevice2Name[] = "Rocksmith USB Guitar Adapter 2";

struct BridgeConfig {
    std::string sourceUID;
    uint32_t sourceChannel = 1;
    uint32_t sourceBufferFrames = kDefaultBufferFrames;
    uint32_t targetLatencyFrames = kDefaultTargetLatencyFrames;
    uint32_t virtualBufferFrames = kDefaultBufferFrames;
};

inline uint32_t clampFrames(uint32_t frames) {
    if (frames < kMinBufferFrames) {
        return kMinBufferFrames;
    }
    if (frames > kMaxTargetLatencyFrames) {
        return kMaxTargetLatencyFrames;
    }
    return frames;
}

inline std::string homeDir() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::string(home);
    }
    passwd* pw = getpwuid(getuid());
    return (pw != nullptr && pw->pw_dir != nullptr) ? std::string(pw->pw_dir) : std::string();
}

inline std::string configDirPath() {
    std::string home = homeDir();
    return home.empty() ? std::string() : home + "/" + kAppSupportDir;
}

inline std::string configPath() {
    std::string dir = configDirPath();
    return dir.empty() ? std::string() : dir + "/" + kConfigFileName;
}

inline std::string cfStringToStdString(CFStringRef value) {
    if (value == nullptr) {
        return {};
    }
    char buffer[1024];
    if (!CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return {};
    }
    return buffer;
}

inline CFStringRef createCFString(const std::string& value) {
    return CFStringCreateWithCString(nullptr, value.c_str(), kCFStringEncodingUTF8);
}

inline uint32_t dictionaryUInt(CFDictionaryRef dict, CFStringRef key, uint32_t fallback) {
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return fallback;
    }
    int number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &number) || number < 0) {
        return fallback;
    }
    return static_cast<uint32_t>(number);
}

inline bool loadConfig(BridgeConfig& config) {
    std::string path = configPath();
    if (path.empty()) {
        return false;
    }
    CFStringRef pathString = createCFString(path);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, pathString, kCFURLPOSIXPathStyle, false);
    CFRelease(pathString);
    if (url == nullptr) {
        return false;
    }

    CFReadStreamRef stream = CFReadStreamCreateWithFile(nullptr, url);
    CFRelease(url);
    if (stream == nullptr || !CFReadStreamOpen(stream)) {
        if (stream != nullptr) {
            CFRelease(stream);
        }
        return false;
    }

    CFPropertyListFormat format = kCFPropertyListXMLFormat_v1_0;
    CFErrorRef error = nullptr;
    CFPropertyListRef plist = CFPropertyListCreateWithStream(nullptr, stream, 0, kCFPropertyListImmutable, &format, &error);
    CFReadStreamClose(stream);
    CFRelease(stream);
    if (error != nullptr) {
        CFRelease(error);
    }
    if (plist == nullptr || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        if (plist != nullptr) {
            CFRelease(plist);
        }
        return false;
    }

    auto dict = static_cast<CFDictionaryRef>(plist);
    CFTypeRef sourceUID = CFDictionaryGetValue(dict, CFSTR("SourceUID"));
    if (sourceUID != nullptr && CFGetTypeID(sourceUID) == CFStringGetTypeID()) {
        config.sourceUID = cfStringToStdString(static_cast<CFStringRef>(sourceUID));
    }
    config.sourceChannel = dictionaryUInt(dict, CFSTR("SourceChannel"), config.sourceChannel);
    if (config.sourceChannel == 0) {
        config.sourceChannel = 1;
    }
    config.sourceBufferFrames = clampFrames(dictionaryUInt(dict, CFSTR("SourceBufferFrames"), config.sourceBufferFrames));
    config.targetLatencyFrames = clampFrames(dictionaryUInt(dict, CFSTR("TargetLatencyFrames"), config.targetLatencyFrames));
    config.virtualBufferFrames = clampFrames(dictionaryUInt(dict, CFSTR("VirtualBufferFrames"), config.virtualBufferFrames));
    CFRelease(plist);
    return true;
}

inline bool ensureConfigDir() {
    std::string home = homeDir();
    std::string dir = configDirPath();
    if (home.empty() || dir.empty()) {
        return false;
    }
    std::string library = home + "/Library";
    std::string appSupport = library + "/Application Support";
    mkdir(library.c_str(), 0755);
    mkdir(appSupport.c_str(), 0755);
    return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

inline void setDictionaryUInt(CFMutableDictionaryRef dict, CFStringRef key, uint32_t value) {
    int number = static_cast<int>(value);
    CFNumberRef cfNumber = CFNumberCreate(nullptr, kCFNumberIntType, &number);
    CFDictionarySetValue(dict, key, cfNumber);
    CFRelease(cfNumber);
}

inline bool saveConfig(const BridgeConfig& config) {
    if (!ensureConfigDir()) {
        return false;
    }

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
    if (!config.sourceUID.empty()) {
        CFStringRef uid = createCFString(config.sourceUID);
        CFDictionarySetValue(dict, CFSTR("SourceUID"), uid);
        CFRelease(uid);
    }
    setDictionaryUInt(dict, CFSTR("SourceChannel"), config.sourceChannel == 0 ? 1 : config.sourceChannel);
    setDictionaryUInt(dict, CFSTR("SourceBufferFrames"), clampFrames(config.sourceBufferFrames));
    setDictionaryUInt(dict, CFSTR("TargetLatencyFrames"), clampFrames(config.targetLatencyFrames));
    setDictionaryUInt(dict, CFSTR("VirtualBufferFrames"), clampFrames(config.virtualBufferFrames));

    std::string path = configPath();
    CFStringRef pathString = createCFString(path);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, pathString, kCFURLPOSIXPathStyle, false);
    CFRelease(pathString);
    if (url == nullptr) {
        CFRelease(dict);
        return false;
    }

    CFWriteStreamRef stream = CFWriteStreamCreateWithFile(nullptr, url);
    CFRelease(url);
    if (stream == nullptr || !CFWriteStreamOpen(stream)) {
        if (stream != nullptr) {
            CFRelease(stream);
        }
        CFRelease(dict);
        return false;
    }

    CFErrorRef error = nullptr;
    CFIndex written = CFPropertyListWrite(dict, stream, kCFPropertyListXMLFormat_v1_0, 0, &error);
    CFWriteStreamClose(stream);
    CFRelease(stream);
    CFRelease(dict);
    if (error != nullptr) {
        CFRelease(error);
    }
    return written > 0;
}

} // namespace rsbridge
