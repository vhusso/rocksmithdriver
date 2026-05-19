SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
CXX := xcrun clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -O2 -Iinclude -isysroot $(SDKROOT) -mmacosx-version-min=14.0
BUILD_DIR := build
DRIVER_BUNDLE := $(BUILD_DIR)/RocksmithMotuBridge.driver
DRIVER_BIN := $(DRIVER_BUNDLE)/Contents/MacOS/RocksmithMotuBridge
HELPER_BIN := $(BUILD_DIR)/bin/RocksmithBridgeHelper
CTL_BIN := $(BUILD_DIR)/bin/rocksmith_bridge_ctl
RTL_BIN := $(BUILD_DIR)/bin/rocksmith_bridge_rtl
UNIT_TEST_BIN := $(BUILD_DIR)/bin/rocksmith_bridge_unit_tests
RENDERED_LAUNCH_AGENT := $(BUILD_DIR)/com.vhusso.rocksmithbridge.helper.plist
HAL_INSTALL_DIR := /Library/Audio/Plug-Ins/HAL
HELPER_INSTALL_DIR := /usr/local/libexec/RocksmithMotuBridge

.PHONY: all driver helper ctl rtl unit-tests bundle sign verify test check clean install-local setup-local create-aggregate

all: bundle helper ctl rtl

driver: $(DRIVER_BIN)

helper: $(HELPER_BIN)

ctl: $(CTL_BIN)

rtl: $(RTL_BIN)

unit-tests: $(UNIT_TEST_BIN)
	"$(UNIT_TEST_BIN)"

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

$(UNIT_TEST_BIN): tests/unit_tests.cpp include/RocksmithBridge/SharedRingBuffer.h include/RocksmithBridge/CoreAudioDeviceUtils.h include/RocksmithBridge/Config.h | $(BUILD_DIR)/bin
	$(CXX) $(CXXFLAGS) -framework CoreAudio -framework CoreFoundation "$<" -o "$@"

$(RENDERED_LAUNCH_AGENT): packaging/com.vhusso.rocksmithbridge.helper.plist | $(BUILD_DIR)/bin
	sed -e "s#__HELPER_PATH__#$(HELPER_INSTALL_DIR)/RocksmithBridgeHelper#g" \
	    -e "s#__HELPER_ERROR_LOG__#$(HOME)/Library/Logs/RocksmithMotuBridge/helper.err#g" \
	    -e "s#__HELPER_OUTPUT_LOG__#$(HOME)/Library/Logs/RocksmithMotuBridge/helper.out#g" \
	    "$<" > "$@"

sign: $(DRIVER_BIN)
	codesign --force --sign - "$(DRIVER_BUNDLE)"

verify: all $(RENDERED_LAUNCH_AGENT)
	codesign --verify --verbose=2 "$(DRIVER_BUNDLE)"
	plutil -lint packaging/Info.plist packaging/com.vhusso.rocksmithbridge.helper.plist "$(RENDERED_LAUNCH_AGENT)" "$(DRIVER_BUNDLE)/Contents/Info.plist"
	zsh -n scripts/install_launch_agent.sh
	zsh -n scripts/uninstall_local.sh

test: verify unit-tests

check: test

install-local: all
	install -d -m 0755 "$(HAL_INSTALL_DIR)" "$(HELPER_INSTALL_DIR)"
	ditto "$(DRIVER_BUNDLE)" "$(HAL_INSTALL_DIR)/RocksmithMotuBridge.driver"
	install -m 0755 "$(HELPER_BIN)" "$(HELPER_INSTALL_DIR)/RocksmithBridgeHelper"
	chown -R root:wheel "$(HAL_INSTALL_DIR)/RocksmithMotuBridge.driver" "$(HELPER_INSTALL_DIR)"
	chmod -R go-w "$(HAL_INSTALL_DIR)/RocksmithMotuBridge.driver" "$(HELPER_INSTALL_DIR)"
	chmod 0755 "$(HELPER_INSTALL_DIR)/RocksmithBridgeHelper"
	killall coreaudiod

setup-local: test
	sudo make install-local
	./scripts/install_launch_agent.sh
	sleep 2
	"$(CTL_BIN)" set-buffers 64
	"$(CTL_BIN)" repair-aggregate
	"$(CTL_BIN)" doctor

create-aggregate: ctl
	"$(CTL_BIN)" repair-aggregate

clean:
	trash "$(BUILD_DIR)" 2>/dev/null || true
