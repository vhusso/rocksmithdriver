#include "RocksmithBridge/CoreAudioDeviceUtils.h"
#include "RocksmithBridge/SharedRingBuffer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

void testFrameClamp() {
    assert(rsbridge::clampFrames(1) == rsbridge::kMinBufferFrames);
    assert(rsbridge::clampFrames(64) == 64);
    assert(rsbridge::clampFrames(999999) == rsbridge::kMaxTargetLatencyFrames);
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

} // namespace

int main() {
    testFrameClamp();
    testPlayerPaths();
    testSourceChannelRange();
    testSharedRingReadWrite();
    std::puts("unit tests passed");
    return 0;
}
