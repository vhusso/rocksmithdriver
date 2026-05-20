#include "RocksmithBridge/CoreAudioDeviceUtils.h"
#include "RocksmithBridge/SharedRingBuffer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void testFrameClamp() {
    assert(rsbridge::clampFrames(1) == rsbridge::kMinBufferFrames);
    assert(rsbridge::clampFrames(64) == 64);
    assert(rsbridge::clampFrames(999999) == rsbridge::kMaxTargetLatencyFrames);
    assert(rsbridge::clampActivePlayerCount(0) == 1);
    assert(rsbridge::clampActivePlayerCount(1) == 1);
    assert(rsbridge::clampActivePlayerCount(2) == 2);
    assert(rsbridge::clampActivePlayerCount(99) == rsbridge::kBridgePlayerCount);
}

void testPlayerPaths() {
    assert(std::strcmp(rsbridge::sharedRingPathForPlayer(1), rsbridge::kSharedMemoryPath) == 0);
    assert(std::strcmp(rsbridge::sharedRingPathForPlayer(2), rsbridge::kSharedMemoryPathPlayer2) == 0);
    assert(rsbridge::sharedRingPathForPlayer(0) == nullptr);
    assert(rsbridge::sharedRingPathForPlayer(3) == nullptr);
}

void testSourceChannelRange() {
    rsbridge::InputDeviceInfo info;
    info.inputChannels = 2;
    assert(rsbridge::hasInputChannelRange(info, 1));
    assert(!rsbridge::hasInputChannelRange(info, 0));
    assert(!rsbridge::hasInputChannelRange(info, 2));
    info.inputChannels = 8;
    assert(rsbridge::hasInputChannelRange(info, 7));
    assert(!rsbridge::hasInputChannelRange(info, 8));
}

void testSharedRingReadWrite() {
    std::string path = "/tmp/com.vhusso.rocksmithbridge.test.";
    path += std::to_string(static_cast<unsigned long long>(getpid()));
    unlink(path.c_str());

    rsbridge::SharedRing ring;
    assert(rsbridge::openSharedRingAtPath(ring, path.c_str(), true));
    rsbridge::setTargetLatencyFrames(ring, 16);

    const float input[] = {0.10f, -0.25f, 0.50f, -0.75f};
    rsbridge::writeMonoFrames(ring, input, 4);

    uint64_t readFrame = 0;
    float output[4] = {};
    uint32_t copied = rsbridge::readMonoFrames(ring, readFrame, output, 4);
    assert(copied == 4);
    for (int i = 0; i < 4; ++i) {
        assert(std::fabs(output[i] - input[i]) < 0.000001f);
    }
    assert(ring.header->heartbeat.load(std::memory_order_acquire) == 1);
    assert(ring.header->driverReadCalls.load(std::memory_order_acquire) == 1);

    rsbridge::closeSharedRing(ring);
    unlink(path.c_str());
}

void testSharedRingReadOnlyLatest() {
    std::string path = "/tmp/com.vhusso.rocksmithbridge.test.readonly.";
    path += std::to_string(static_cast<unsigned long long>(getpid()));
    unlink(path.c_str());

    rsbridge::SharedRing writer;
    assert(rsbridge::openSharedRingAtPath(writer, path.c_str(), true));
    rsbridge::setTargetLatencyFrames(writer, 16);
    float input[20] = {};
    for (int i = 0; i < 20; ++i) {
        input[i] = static_cast<float>(i) / 100.0f;
    }
    rsbridge::writeMonoFrames(writer, input, 20);
    rsbridge::closeSharedRing(writer);

    rsbridge::SharedRing reader;
    assert(rsbridge::openSharedRingAtPath(reader, path.c_str(), false, nullptr, rsbridge::SharedRingAccess::readOnly));
    float output[2] = {};
    assert(rsbridge::readLatestMonoFrames(reader, output, 2) == 2);
    assert(std::fabs(output[0] - input[4]) < 0.000001f);
    assert(std::fabs(output[1] - input[5]) < 0.000001f);
    assert(!reader.writable);
    rsbridge::closeSharedRing(reader);
    unlink(path.c_str());
}

void testSharedRingRejectsUnsafeFiles() {
    std::string path = "/tmp/com.vhusso.rocksmithbridge.test.unsafe.";
    path += std::to_string(static_cast<unsigned long long>(getpid()));
    unlink(path.c_str());

    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0666);
    assert(fd >= 0);
    assert(fchmod(fd, 0666) == 0);
    assert(ftruncate(fd, static_cast<off_t>(rsbridge::sharedRingSize())) == 0);
    close(fd);

    rsbridge::SharedRing ring;
    rsbridge::SharedRingOpenError error = rsbridge::SharedRingOpenError::none;
    assert(!rsbridge::openSharedRingAtPath(ring, path.c_str(), false, &error, rsbridge::SharedRingAccess::readOnly));
    assert(error == rsbridge::SharedRingOpenError::invalidPermissions);
    unlink(path.c_str());

    fd = open(path.c_str(), O_CREAT | O_RDWR, rsbridge::kSharedRingFileMode);
    assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);
    close(fd);
    error = rsbridge::SharedRingOpenError::none;
    assert(!rsbridge::openSharedRingAtPath(ring, path.c_str(), false, &error, rsbridge::SharedRingAccess::readOnly));
    assert(error == rsbridge::SharedRingOpenError::invalidSize);
    unlink(path.c_str());
}

} // namespace

int main() {
    testFrameClamp();
    testPlayerPaths();
    testSourceChannelRange();
    testSharedRingReadWrite();
    testSharedRingReadOnlyLatest();
    testSharedRingRejectsUnsafeFiles();
    std::puts("unit tests passed");
    return 0;
}
