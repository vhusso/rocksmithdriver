# Contributing

This project is a local-development macOS Core Audio driver stack. Changes should keep the HAL plug-in minimal and keep Core Audio client work in user processes.

## Development Loop

```sh
make test
```

`make check` is kept as an alias for the same gate.

For installed-driver checks:

```sh
sudo make install-local
./scripts/install_launch_agent.sh
./build/bin/rocksmith_bridge_ctl repair-aggregate
./build/bin/rocksmith_bridge_ctl doctor
```

## Design Rules

- Do not call Core Audio client HAL APIs from inside the AudioServerPlugIn.
- Keep MOTU/source capture in `RocksmithBridgeHelper`.
- Preserve silence-on-failure behavior in the driver.
- Preserve latest-audio ring behavior; do not introduce queued stale-buffer playback.
- Keep defaults conservative: 48 kHz, mono, 64 frames.

## Commit Style

Use Conventional Commits:

```text
feat: add player two bridge source
fix: recover helper after source disconnect
docs: document round-trip measurement
```
