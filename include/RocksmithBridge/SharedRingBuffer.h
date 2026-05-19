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
    invalidFile,
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
        case SharedRingOpenError::invalidFile:
            return "invalid file";
        case SharedRingOpenError::invalidHeader:
            return "invalid header";
    }
}

inline size_t sharedRingSize() {
    return sizeof(SharedRingHeader) + sizeof(float) * kRingCapacityFrames * kChannelCount;
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
    return player <= 1 ? kSharedMemoryPath : kSharedMemoryPathPlayer2;
}

inline bool openSharedRingAtPath(SharedRing& ring, const char* path, bool createIfMissing,
                                 SharedRingOpenError* error = nullptr) {
    if (error != nullptr) {
        *error = SharedRingOpenError::none;
    }
    int flags = createIfMissing ? (O_CREAT | O_RDWR) : O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    ring.fd = open(path, flags, 0666);
    if (ring.fd < 0) {
        if (error != nullptr) {
            *error = SharedRingOpenError::openFailed;
        }
        return false;
    }

    struct stat statBuffer {};
    if (fstat(ring.fd, &statBuffer) != 0 || !S_ISREG(statBuffer.st_mode)) {
        if (error != nullptr) {
            *error = SharedRingOpenError::invalidFile;
        }
        close(ring.fd);
        ring.fd = -1;
        return false;
    }

    ring.mappingSize = sharedRingSize();
    if (createIfMissing) {
        if (ftruncate(ring.fd, static_cast<off_t>(ring.mappingSize)) != 0) {
            if (error != nullptr) {
                *error = SharedRingOpenError::truncateFailed;
            }
            close(ring.fd);
            ring.fd = -1;
            return false;
        }
        fchmod(ring.fd, 0666);
    }

    ring.mapping = mmap(nullptr, ring.mappingSize, PROT_READ | PROT_WRITE, MAP_SHARED, ring.fd, 0);
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
                                    SharedRingOpenError* error = nullptr) {
    return openSharedRingAtPath(ring, sharedRingPathForPlayer(player), createIfMissing, error);
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
    if (!ring.valid() || input == nullptr) {
        return;
    }

    const uint32_t capacity = ring.header->capacityFrames;
    uint64_t writeFrame = ring.header->writeFrame.load(std::memory_order_relaxed);
    float peak = 0.0f;
    for (uint32_t i = 0; i < frameCount; ++i) {
        const float sample = input[i];
        const float absSample = sample < 0.0f ? -sample : sample;
        if (absSample > peak) {
            peak = absSample;
        }
        ring.samples[(writeFrame + i) % capacity] = sample;
    }

    ring.header->writeFrame.store(writeFrame + frameCount, std::memory_order_release);
    ring.header->heartbeat.fetch_add(1, std::memory_order_relaxed);
    const auto peakPpm = static_cast<uint32_t>((peak > 1.0f ? 1.0f : peak) * 1000000.0f);
    ring.header->inputPeakPpm.store(peakPpm, std::memory_order_relaxed);
}

inline void setTargetLatencyFrames(SharedRing& ring, uint32_t frames) {
    if (!ring.valid()) {
        return;
    }
    if (frames < kMinBufferFrames) {
        frames = kMinBufferFrames;
    }
    if (frames > kMaxTargetLatencyFrames) {
        frames = kMaxTargetLatencyFrames;
    }
    ring.header->targetLatencyFrames.store(frames, std::memory_order_release);
}

inline uint32_t readMonoFrames(SharedRing& ring, uint64_t& readFrame, float* output, uint32_t frameCount) {
    if (!ring.valid() || output == nullptr) {
        if (output != nullptr) {
            std::memset(output, 0, sizeof(float) * frameCount);
        }
        return 0;
    }

    const uint32_t capacity = ring.header->capacityFrames;
    const uint64_t writeFrame = ring.header->writeFrame.load(std::memory_order_acquire);
    const uint32_t targetLatencyFrames = ring.header->targetLatencyFrames.load(std::memory_order_acquire);
    const uint64_t target = targetLatencyFrames > 0 ? targetLatencyFrames : 1;
    const bool invalidReadHead = readFrame == 0 || writeFrame < readFrame || writeFrame - readFrame > capacity;
    const bool latencyDriftedHigh = !invalidReadHead && writeFrame - readFrame > target + frameCount;
    if (invalidReadHead || latencyDriftedHigh) {
        readFrame = writeFrame > target ? writeFrame - target : 0;
        ring.header->overruns.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t available = writeFrame - readFrame;
    uint32_t copied = 0;
    while (copied < frameCount && available > 0) {
        output[copied] = ring.samples[readFrame % capacity];
        ++readFrame;
        ++copied;
        --available;
    }

    if (copied < frameCount) {
        std::memset(output + copied, 0, sizeof(float) * (frameCount - copied));
        ring.header->underruns.fetch_add(1, std::memory_order_relaxed);
    }
    ring.header->driverReadCalls.fetch_add(1, std::memory_order_relaxed);
    ring.header->driverReadFrames.fetch_add(frameCount, std::memory_order_relaxed);
    return copied;
}

} // namespace rsbridge
