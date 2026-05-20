# Rocksmith Audio Input Bridge

Local macOS Core Audio bridge for using audio-interface inputs as Rocksmith 2014 cable-like inputs. It defaults to MOTU M4 input 1 for player 1 when no config exists, but it can be configured for one or two active players from any Core Audio input device with enough adjacent input channels.

The project has four pieces:

- `RocksmithMotuBridge.driver`: AudioServerPlugIn HAL driver exposing two mono virtual inputs named `Rocksmith MOTU Bridge Source 1` and `Rocksmith MOTU Bridge Source 2`.
- `RocksmithBridgeHelper`: user process that captures the configured active input channel or adjacent channel pair and writes mono float32 audio into shared rings.
- `rocksmith_bridge_ctl`: setup and diagnostics tool for config, buffers, and the aggregates named `Rocksmith USB Guitar Adapter 1` and `Rocksmith USB Guitar Adapter 2`.
- `rocksmith_bridge_rtl`: round-trip latency measurement tool for physical loopback tests.

## Build

Requirements:

- macOS 14 or newer.
- Apple Command Line Tools (`xcode-select --install`).
- Admin access for the local HAL install.

This is currently a source-build local install. It is not a signed or notarized public binary installer.

```sh
make all
```

The build uses Command Line Tools and produces artifacts under `build/`.

Run local quality checks:

```sh
make test
```

## Local Install

The easiest local setup path is:

```sh
make setup-local
```

That runs the local quality gate, installs the HAL bundle and helper, loads the launch agent, applies the known-good `64` frame buffers, repairs the player 1 Rocksmith aggregate device, and runs `doctor`.

For a manual install, `install-local` copies the HAL bundle into `/Library/Audio/Plug-Ins/HAL`, installs the helper under `/usr/local/libexec`, enforces root-owned install permissions, and restarts `coreaudiod`.

```sh
sudo make install-local
./scripts/install_launch_agent.sh
./build/bin/rocksmith_bridge_ctl repair-aggregate
./build/bin/rocksmith_bridge_ctl doctor
```

Then open Audio MIDI Setup and verify:

- `Rocksmith MOTU Bridge Source 1` exists and has 1 input channel.
- `Rocksmith USB Guitar Adapter 1` exists and has 1 input channel.

Launch Rocksmith 2014 after those devices are visible. The default setup keeps player 1 only because that is the stable Rocksmith test path and avoids idle player 2 work. Enable two active players explicitly before testing adapter 2.

## Useful Commands

```sh
./build/bin/rocksmith_bridge_ctl list-inputs
./build/bin/rocksmith_bridge_ctl choose-source
./build/bin/rocksmith_bridge_ctl set-source DEVICE_UID CHANNEL
./build/bin/rocksmith_bridge_ctl set-active-players 1
./build/bin/rocksmith_bridge_ctl set-active-players 2
./build/bin/rocksmith_bridge_ctl get-config
./build/bin/rocksmith_bridge_ctl set-buffers 64
./build/bin/rocksmith_bridge_ctl doctor
./build/bin/rocksmith_bridge_ctl status
./build/bin/rocksmith_bridge_ctl set-bridge-latency 16
./build/bin/rocksmith_bridge_ctl set-virtual-buffer 16
./build/bin/rocksmith_bridge_ctl repair-aggregate
./build/bin/rocksmith_bridge_ctl destroy-aggregate
tail -f ~/Library/Logs/RocksmithMotuBridge/helper.err
```

For battery life, stop the helper when you are done playing:

```sh
make stop-helper
```

Start it again before playing:

```sh
make start-helper
```

`list-devices` remains as a compatibility alias for `list-inputs`.

Config lives at:

```sh
~/Library/Application Support/RocksmithMotuBridge/config.plist
```

Use `set-source` only when you want something other than the automatic MOTU M4 input 1 default. Use the UID printed by `list-inputs`; the channel argument is the first input channel used for player 1. When two active players are enabled, the next adjacent channel feeds player 2.

## Player Count

The low-power default is one active player:

```sh
./build/bin/rocksmith_bridge_ctl set-active-players 1
./build/bin/rocksmith_bridge_ctl repair-aggregate
```

To test two Rocksmith adapters from one interface, use adjacent interface inputs. For example, with MOTU M4 inputs 1 and 2:

```sh
./build/bin/rocksmith_bridge_ctl set-active-players 2
./build/bin/rocksmith_bridge_ctl set-source com_motu_driver_coreuac_control_interface:m4ma0f8aty 1
./build/bin/rocksmith_bridge_ctl repair-aggregate-all
```

Player 1 receives the configured channel. Player 2 receives `CHANNEL + 1`. Return to `set-active-players 1` when you want the helper to stop feeding adapter 2.

## Other Audio Interfaces

MOTU M4 is only the built-in default because it is the original tested setup. The helper can capture from any Core Audio input device listed by:

```sh
./build/bin/rocksmith_bridge_ctl list-inputs
```

Configure a different interface with its device UID and first channel:

```sh
./build/bin/rocksmith_bridge_ctl choose-source
./build/bin/rocksmith_bridge_ctl set-source DEVICE_UID CHANNEL
```

`choose-source` is the guided path and avoids copy-pasting long device UIDs. With one active player, single-input interfaces are supported. With two active players, the selected device must have an adjacent channel pair starting at `CHANNEL`.

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

The bridge does not intentionally queue multiple buffers. The virtual driver reads the latest complete window behind the helper write head, using the larger of the configured bridge safety latency and the current render quantum. That keeps multiple Core Audio clients from consuming or stealing frames from each other.

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

The uninstall script destroys the managed aggregate devices, unloads the launch agent, moves the installed helper and HAL bundle into the invoking user's Trash, then restarts `coreaudiod`.

## Notes

- The HAL driver intentionally does not open the MOTU device. AudioServerPlugIns run inside a restricted audio host, and Apple warns against calling client HAL APIs from there.
- The helper is the only Core Audio client for the MOTU M4.
- The shared audio format between helper and driver is 48 kHz mono float32.
- The virtual stream advertises 48 kHz mono float32 and 48 kHz mono signed 16-bit.
