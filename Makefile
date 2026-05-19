SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
CXX := xcrun clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -O2 -Iinclude -isysroot $(SDKROOT) -mmacosx-version-min=14.0
BUILD_DIR := build
DRIVER_BUNDLE := $(BUILD_DIR)/RocksmithMotuBridge.driver
DRIVER_BIN := $(DRIVER_BUNDLE)/Contents/MacOS/RocksmithMotuBridge
HELPER_BIN := $(BUILD_DIR)/bin/RocksmithBridgeHelper
CTL_BIN := $(BUILD_DIR)/bin/rocksmith_bridge_ctl
RTL_BIN := $(BUILD_DIR)/bin/rocksmith_bridge_rtl

.PHONY: all driver helper ctl rtl bundle sign clean install-local create-aggregate

all: bundle helper ctl rtl

driver: $(DRIVER_BIN)

helper: $(HELPER_BIN)

ctl: $(CTL_BIN)

rtl: $(RTL_BIN)

bundle: $(DRIVER_BIN) $(DRIVER_BUNDLE)/Contents/Info.plist sign

$(DRIVER_BUNDLE)/Contents/MacOS:
	mkdir -p "$@"

$(BUILD_DIR)/bin:
	mkdir -p "$@"

$(DRIVER_BUNDLE)/Contents/Info.plist: packaging/Info.plist
	mkdir -p "$(DRIVER_BUNDLE)/Contents"
	cp "$<" "$@"

$(DRIVER_BIN): src/driver/RocksmithBridgeDriver.cpp src/driver/RocksmithBridgeDriverPlugin.inc include/RocksmithBridge/SharedRingBuffer.h | $(DRIVER_BUNDLE)/Contents/MacOS
	$(CXX) $(CXXFLAGS) -fvisibility=hidden -bundle -framework CoreAudio -framework CoreFoundation "$<" -o "$@"

$(HELPER_BIN): src/helper/RocksmithBridgeHelper.cpp include/RocksmithBridge/SharedRingBuffer.h include/RocksmithBridge/Config.h include/RocksmithBridge/CoreAudioDeviceUtils.h | $(BUILD_DIR)/bin
	$(CXX) $(CXXFLAGS) -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreFoundation "$<" -o "$@"

$(CTL_BIN): src/tools/rocksmith_bridge_ctl.cpp include/RocksmithBridge/SharedRingBuffer.h include/RocksmithBridge/Config.h include/RocksmithBridge/CoreAudioDeviceUtils.h | $(BUILD_DIR)/bin
	$(CXX) $(CXXFLAGS) -framework CoreAudio -framework CoreFoundation "$<" -o "$@"

$(RTL_BIN): src/tools/rocksmith_bridge_rtl.cpp include/RocksmithBridge/Config.h include/RocksmithBridge/CoreAudioDeviceUtils.h | $(BUILD_DIR)/bin
	$(CXX) $(CXXFLAGS) -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreFoundation "$<" -o "$@"

sign: $(DRIVER_BIN)
	codesign --force --sign - "$(DRIVER_BUNDLE)"

install-local: all
	install -d "/Library/Audio/Plug-Ins/HAL"
	cp -R "$(DRIVER_BUNDLE)" "/Library/Audio/Plug-Ins/HAL/"
	killall coreaudiod

create-aggregate: ctl
	"$(CTL_BIN)" repair-aggregate

clean:
	trash "$(BUILD_DIR)" 2>/dev/null || true
