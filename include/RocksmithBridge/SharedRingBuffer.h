#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rsbridge {

inline constexpr char kSharedMemoryPath[] = "/tmp/com.vhusso.rocksmithbridge.audio";
inline constexpr char kSharedMemoryPathPlayer2[] = "/tmp/com.vhusso.rocksmithbridge.audio.2";
inline constexpr mode_t kSharedRingFileMode = 0644;
inline constexpr uint32_t kSharedRingMagic = 0x52534252; // RSBR
inline constexpr uint32_t kSharedRingVersion = 3;
inline constexpr uint32_t kBridgePlayerCount = 2;
inline constexpr uint32_t kSampleRate = 48000;
inline constexpr uint32_t kChannelCount = 1;
inline constexpr uint32_t kRingCapacityFrames = kSampleRate * 4;
inline constexpr uint32_t kMinBufferFrames = 16;
inline constexpr uint32_t kDefaultBufferFrames = 64;
inline constexpr uint32_t kDefaultTargetLatencyFrames = 64;
inline constexpr uint32_t kMaxTargetLatencyFrames = 2048;

struct SharedRingHeader {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    uint32_t capacityFrames;
    uint32_t sampleRate;
    uint32_t channelCount;
    std::atomic<uint32_t> targetLatencyFrames;
    std::atomic<uint64_t> writeFrame;
    std::atomic<uint64_t> heartbeat;
    std::atomic<uint64_t> underruns;
    std::atomic<uint64_t> overruns;
    std::atomic<uint64_t> driverReadCalls;
    std::atomic<uint64_t> driverReadFrames;
    std::atomic<uint32_t> inputPeakPpm;
};

struct SharedRing {
    int fd = -1;
    void* mapping = MAP_FAILED;
    size_t mappingSize = 0;
    SharedRingHeader* header = nullptr;
    float* samples = nullptr;
    bool writable = false;

    bool valid() const {
        return header != nullptr && samples != nullptr &&
               header->magic.load(std::memory_order_acquire) == kSharedRingMagic &&
               header->version.load(std::memory_order_acquire) == kSharedRingVersion &&
               header->capacityFrames == kRingCapacityFrames &&
               header->channelCount == kChannelCount;
    }
};

enum class SharedRingOpenError {
    none,
    openFailed,
    truncateFailed,
    mmapFailed,
    invalidPlayer,
    invalidFile,
    invalidSize,
    invalidPermissions,
    invalidHeader
};

inline const char* sharedRingOpenErrorMessage(SharedRingOpenError error) {
    switch (error) {
        case SharedRingOpenError::none:
            return "none";
        case SharedRingOpenError::openFailed:
            return "open failed";
        case SharedRingOpenError::truncateFailed:
            return "ftruncate failed";
        case SharedRingOpenError::mmapFailed:
            return "mmap failed";
        case SharedRingOpenError::invalidPlayer:
            return "invalid player";
        case SharedRingOpenError::invalidFile:
            return "invalid file";
        case SharedRingOpenError::invalidSize:
            return "invalid size";
        case SharedRingOpenError::invalidPermissions:
            return "invalid permissions";
        case SharedRingOpenError::invalidHeader:
            return "invalid header";
    }
}

enum class SharedRingAccess {
    readOnly,
    readWrite
};

inline size_t sharedRingSize() {
    return sizeof(SharedRingHeader) + sizeof(float) * kRingCapacityFrames * kChannelCount;
}

inline uint32_t clampTargetLatencyFrames(uint32_t frames) {
    if (frames < kMinBufferFrames) {
        return kMinBufferFrames;
    }
    if (frames > kMaxTargetLatencyFrames) {
        return kMaxTargetLatencyFrames;
    }
    return frames;
}

inline void initializeSharedRing(SharedRingHeader* header) {
    header->magic.store(0, std::memory_order_release);
    header->version.store(kSharedRingVersion, std::memory_order_release);
    header->capacityFrames = kRingCapacityFrames;
    header->sampleRate = kSampleRate;
    header->channelCount = kChannelCount;
    header->targetLatencyFrames.store(kDefaultTargetLatencyFrames, std::memory_order_release);
    header->writeFrame.store(0, std::memory_order_release);
    header->heartbeat.store(0, std::memory_order_release);
    header->underruns.store(0, std::memory_order_release);
    header->overruns.store(0, std::memory_order_release);
    header->driverReadCalls.store(0, std::memory_order_release);
    header->driverReadFrames.store(0, std::memory_order_release);
    header->inputPeakPpm.store(0, std::memory_order_release);
    header->magic.store(kSharedRingMagic, std::memory_order_release);
}

inline const char* sharedRingPathForPlayer(uint32_t player) {
    if (player == 1) {
        return kSharedMemoryPath;
    }
    if (player == 2) {
        return kSharedMemoryPathPlayer2;
    }
    return nullptr;
}

inline bool openSharedRingAtPath(SharedRing& ring, const char* path, bool createIfMissing,
                                 SharedRingOpenError* error = nullptr,
                                 SharedRingAccess access = SharedRingAccess::readWrite) {
    if (error != nullptr) {
        *error = SharedRingOpenError::none;
    }
    if (path == nullptr) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidFile;
        }
        return false;
    }
    const bool writable = access == SharedRingAccess::readWrite;
    int flags = createIfMissing || writable ? O_RDWR : O_RDONLY;
    if (createIfMissing) {
        flags |= O_CREAT;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    ring.fd = open(path, flags, kSharedRingFileMode);
    if (ring.fd < 0) {
        if (error != nullptr) {
            *error = SharedRingOpenError::openFailed;
        }
        return false;
    }

    ring.mappingSize = sharedRingSize();
    struct stat statBuffer {};
    if (fstat(ring.fd, &statBuffer) != 0 || !S_ISREG(statBuffer.st_mode)) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidFile;
        }
        close(ring.fd);
        ring.fd = -1;
        return false;
    }
    if (statBuffer.st_nlink != 1) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidPermissions;
        }
        close(ring.fd);
        ring.fd = -1;
        return false;
    }
    if ((statBuffer.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        if (!createIfMissing || fchmod(ring.fd, kSharedRingFileMode) != 0) {
            if (error != nullptr) {
                *error = SharedRingOpenError::invalidPermissions;
            }
            close(ring.fd);
            ring.fd = -1;
            return false;
        }
    }

    if (createIfMissing) {
        if (ftruncate(ring.fd, static_cast<off_t>(ring.mappingSize)) != 0) {
            if (error != nullptr) {
                *error = SharedRingOpenError::truncateFailed;
            }
            close(ring.fd);
            ring.fd = -1;
            return false;
        }
        fchmod(ring.fd, kSharedRingFileMode);
    } else if (statBuffer.st_size != static_cast<off_t>(ring.mappingSize)) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidSize;
        }
        close(ring.fd);
        ring.fd = -1;
        return false;
    }

    const int protections = PROT_READ | (writable ? PROT_WRITE : 0);
    ring.mapping = mmap(nullptr, ring.mappingSize, protections, MAP_SHARED, ring.fd, 0);
    if (ring.mapping == MAP_FAILED) {
        if (error != nullptr) {
            *error = SharedRingOpenError::mmapFailed;
        }
        close(ring.fd);
        ring.fd = -1;
        return false;
    }

    ring.header = static_cast<SharedRingHeader*>(ring.mapping);
    ring.samples = reinterpret_cast<float*>(static_cast<uint8_t*>(ring.mapping) + sizeof(SharedRingHeader));
    ring.writable = writable;

    const bool needsInit = ring.header->magic.load(std::memory_order_acquire) != kSharedRingMagic ||
                           ring.header->version.load(std::memory_order_acquire) != kSharedRingVersion ||
                           ring.header->capacityFrames != kRingCapacityFrames ||
                           ring.header->channelCount != kChannelCount;
    if (createIfMissing && needsInit) {
        std::memset(ring.mapping, 0, ring.mappingSize);
        initializeSharedRing(ring.header);
    }

    if (!ring.valid()) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidHeader;
        }
        if (ring.mapping != MAP_FAILED) {
            munmap(ring.mapping, ring.mappingSize);
        }
        if (ring.fd >= 0) {
            close(ring.fd);
        }
        ring = SharedRing{};
        return false;
    }
    return true;
}

inline bool openSharedRing(SharedRing& ring, bool createIfMissing, SharedRingOpenError* error = nullptr) {
    return openSharedRingAtPath(ring, kSharedMemoryPath, createIfMissing, error);
}

inline bool openSharedRingForPlayer(SharedRing& ring, uint32_t player, bool createIfMissing,
                                    SharedRingOpenError* error = nullptr,
                                    SharedRingAccess access = SharedRingAccess::readWrite) {
    const char* path = sharedRingPathForPlayer(player);
    if (path == nullptr) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidPlayer;
        }
        return false;
    }
    return openSharedRingAtPath(ring, path, createIfMissing, error, access);
}

inline void closeSharedRing(SharedRing& ring) {
    if (ring.mapping != MAP_FAILED) {
        munmap(ring.mapping, ring.mappingSize);
    }
    if (ring.fd >= 0) {
        close(ring.fd);
    }
    ring = SharedRing{};
}

inline void writeMonoFrames(SharedRing& ring, const float* input, uint32_t frameCount) {
    if (!ring.valid() || !ring.writable || input == nullptr) {
        return;
    }

    uint64_t writeFrame = ring.header->writeFrame.load(std::memory_order_relaxed);
    float peak = 0.0f;
    for (uint32_t i = 0; i < frameCount; ++i) {
        const float sample = input[i];
        const float absSample = sample < 0.0f ? -sample : sample;
        if (absSample > peak) {
            peak = absSample;
        }
        ring.samples[(writeFrame + i) % kRingCapacityFrames] = sample;
    }

    ring.header->writeFrame.store(writeFrame + frameCount, std::memory_order_release);
    ring.header->heartbeat.fetch_add(1, std::memory_order_relaxed);
    const auto peakPpm = static_cast<uint32_t>((peak > 1.0f ? 1.0f : peak) * 1000000.0f);
    ring.header->inputPeakPpm.store(peakPpm, std::memory_order_relaxed);
}

inline void setTargetLatencyFrames(SharedRing& ring, uint32_t frames) {
    if (!ring.valid() || !ring.writable) {
        return;
    }
    ring.header->targetLatencyFrames.store(clampTargetLatencyFrames(frames), std::memory_order_release);
}

inline uint32_t readMonoFrames(SharedRing& ring, uint64_t& readFrame, float* output, uint32_t frameCount) {
    if (!ring.valid() || output == nullptr) {
        if (output != nullptr) {
            std::memset(output, 0, sizeof(float) * frameCount);
        }
        return 0;
    }

    const uint64_t writeFrame = ring.header->writeFrame.load(std::memory_order_acquire);
    const uint32_t targetLatencyFrames = clampTargetLatencyFrames(ring.header->targetLatencyFrames.load(std::memory_order_acquire));
    const uint64_t target = targetLatencyFrames > 0 ? targetLatencyFrames : 1;
    const bool invalidReadHead = readFrame == 0 || writeFrame < readFrame || writeFrame - readFrame > kRingCapacityFrames;
    const bool latencyDriftedHigh = !invalidReadHead && writeFrame - readFrame > target + frameCount;
    if (invalidReadHead || latencyDriftedHigh) {
        readFrame = writeFrame > target ? writeFrame - target : 0;
        if (ring.writable) {
            ring.header->overruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t available = writeFrame - readFrame;
    uint32_t copied = 0;
    while (copied < frameCount && available > 0) {
        output[copied] = ring.samples[readFrame % kRingCapacityFrames];
        ++readFrame;
        ++copied;
        --available;
    }

    if (copied < frameCount) {
        std::memset(output + copied, 0, sizeof(float) * (frameCount - copied));
        if (ring.writable) {
            ring.header->underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (ring.writable) {
        ring.header->driverReadCalls.fetch_add(1, std::memory_order_relaxed);
        ring.header->driverReadFrames.fetch_add(frameCount, std::memory_order_relaxed);
    }
    return copied;
}

inline uint32_t readLatestMonoFrames(SharedRing& ring, float* output, uint32_t frameCount) {
    if (!ring.valid() || output == nullptr) {
        if (output != nullptr) {
            std::memset(output, 0, sizeof(float) * frameCount);
        }
        return 0;
    }

    const uint64_t writeFrame = ring.header->writeFrame.load(std::memory_order_acquire);
    const uint32_t targetLatencyFrames = clampTargetLatencyFrames(ring.header->targetLatencyFrames.load(std::memory_order_acquire));
    const uint64_t lag = targetLatencyFrames > frameCount ? targetLatencyFrames : frameCount;
    uint64_t readFrame = writeFrame > lag ? writeFrame - lag : 0;
    uint64_t available = writeFrame - readFrame;
    uint32_t copied = 0;
    while (copied < frameCount && available > 0) {
        output[copied] = ring.samples[readFrame % kRingCapacityFrames];
        ++readFrame;
        ++copied;
        --available;
    }
    if (copied < frameCount) {
        std::memset(output + copied, 0, sizeof(float) * (frameCount - copied));
        if (ring.writable) {
            ring.header->underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (ring.writable) {
        ring.header->driverReadCalls.fetch_add(1, std::memory_order_relaxed);
        ring.header->driverReadFrames.fetch_add(frameCount, std::memory_order_relaxed);
    }
    return copied;
}

} // namespace rsbridge
