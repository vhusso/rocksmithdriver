# Rocksmith MOTU Input Bridge

Local macOS Core Audio bridge for using MOTU M4 inputs as Rocksmith 2014 cable-like inputs. It defaults to MOTU M4 input 1 for player 1 and input 2 for player 2 when no config exists.

The project has three pieces:

- `RocksmithMotuBridge.driver`: AudioServerPlugIn HAL driver exposing two mono virtual inputs named `Rocksmith MOTU Bridge Source 1` and `Rocksmith MOTU Bridge Source 2`.
- `RocksmithBridgeHelper`: user process that captures the configured input channel pair and writes mono float32 audio into two shared rings.
- `rocksmith_bridge_ctl`: setup and diagnostics tool for config, buffers, and the aggregates named `Rocksmith USB Guitar Adapter 1` and `Rocksmith USB Guitar Adapter 2`.
- `rocksmith_bridge_rtl`: round-trip latency measurement tool for physical loopback tests.

## Build

```sh
make all
```

The build uses Command Line Tools and produces artifacts under `build/`.

Run local quality checks:

```sh
make test
```

## Local Install

```sh
sudo make install-local
./scripts/install_launch_agent.sh
./build/bin/rocksmith_bridge_ctl repair-aggregate
./build/bin/rocksmith_bridge_ctl doctor
```

Then open Audio MIDI Setup and verify:

- `Rocksmith MOTU Bridge Source 1` exists and has 1 input channel.
- `Rocksmith MOTU Bridge Source 2` exists and has 1 input channel.
- `Rocksmith USB Guitar Adapter 1` exists and has 1 input channel.
- `Rocksmith USB Guitar Adapter 2` exists and has 1 input channel.

Launch Rocksmith 2014 after those devices are visible.

## Useful Commands

```sh
./build/bin/rocksmith_bridge_ctl list-inputs
./build/bin/rocksmith_bridge_ctl set-source DEVICE_UID CHANNEL
./build/bin/rocksmith_bridge_ctl get-config
./build/bin/rocksmith_bridge_ctl set-buffers 64
./build/bin/rocksmith_bridge_ctl doctor
./build/bin/rocksmith_bridge_ctl status
./build/bin/rocksmith_bridge_ctl set-bridge-latency 16
./build/bin/rocksmith_bridge_ctl set-virtual-buffer 16
./build/bin/rocksmith_bridge_ctl repair-aggregate
./build/bin/rocksmith_bridge_ctl destroy-aggregate
tail -f /tmp/rocksmithbridge-helper.err
```

`list-devices` remains as a compatibility alias for `list-inputs`.

Config lives at:

```sh
~/Library/Application Support/RocksmithMotuBridge/config.plist
```

Use `set-source` only when you want something other than the automatic MOTU M4 input 1/2 default. Use the UID printed by `list-inputs`; the channel argument is the first channel in the pair, so `set-source DEVICE_UID 1` captures channels 1 and 2.

## Latency and Buffers

There are three separate buffer/latency controls:

- Source capture buffer: requested by the helper from the selected Core Audio input device, default `64`, accepted range `16` to `2048`.
- Bridge safety latency: how far the virtual driver reads behind the helper, default `64`, accepted range `16` to `2048`.
- Rocksmith/Core Audio buffer: Rocksmith can request a buffer size from the virtual driver. The driver advertises a settable `kAudioDevicePropertyBufferFrameSize` range of `16` to `2048` frames and defaults to `64`.

At 48 kHz:

- 16 frames = 0.33 ms
- 64 frames = 1.33 ms
- 128 frames = 2.67 ms
- 256 frames = 5.33 ms

The known-good baseline is:

```sh
./build/bin/rocksmith_bridge_ctl set-buffers 64
```

with Rocksmith Audio Engine Setting `2`.

The lower-latency experiments are:

```sh
./build/bin/rocksmith_bridge_ctl set-buffers 32
./build/bin/rocksmith_bridge_ctl set-buffers 16
```

If you hear crackles or the `status` command shows underruns increasing while playing, go back to `64` first. If it is stable, try lowering Rocksmith's own audio engine buffer.

`set-virtual-buffer` asks Core Audio to set the virtual device buffer. Rocksmith may still request its own size when it opens the device, and its in-game audio engine setting is separate from this driver property.

The bridge does not intentionally queue multiple buffers. The shared ring is treated as a latest-audio handoff: if the virtual driver sees more than the configured safety latency plus the current render quantum, it resynchronizes near the writer instead of draining old audio.

## Round-Trip Latency Measurement

The RTL tool sends a short impulse to an output channel and measures when it arrives at an input channel. Use a physical cable from a MOTU line output to the input you want to test. Start with monitor volume low, input gain low, and avoid phantom power.

List devices:

```sh
./build/bin/rocksmith_bridge_rtl --list
```

Raw MOTU hardware/Core Audio RTL, with MOTU output 1 cabled to MOTU input 1:

```sh
./build/bin/rocksmith_bridge_rtl com_motu_driver_coreuac_control_interface:m4ma0f8aty 1 com_motu_driver_coreuac_control_interface:m4ma0f8aty 1
```

Full bridge path, with the same cable but measuring what Rocksmith would see through the aggregate:

```sh
./build/bin/rocksmith_bridge_rtl com_motu_driver_coreuac_control_interface:m4ma0f8aty 1 com.vhusso.rocksmithbridge.aggregate 1
```

The difference between those two numbers is the bridge-side cost. For the full bridge path, keep the helper running and set the bridge buffers first:

```sh
./build/bin/rocksmith_bridge_ctl set-buffers 64
./build/bin/rocksmith_bridge_ctl doctor
```

## Uninstall

```sh
sudo ./scripts/uninstall_local.sh
```

The uninstall script uses `trash` for removals, unloads the launch agent, removes the installed helper and HAL bundle, then restarts `coreaudiod`.

## Notes

- The HAL driver intentionally does not open the MOTU device. AudioServerPlugIns run inside a restricted audio host, and Apple warns against calling client HAL APIs from there.
- The helper is the only Core Audio client for the MOTU M4.
- The shared audio format between helper and driver is 48 kHz mono float32.
- The virtual stream advertises 48 kHz mono float32 and 48 kHz mono signed 16-bit.
