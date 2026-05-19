# Security Policy

This repository builds a local macOS audio driver stack. It is not currently distributed as a signed or notarized public installer.

## Reporting

Report security issues privately to the repository owner. Do not open public issues for vulnerabilities involving privilege boundaries, driver loading, launch agents, file permissions, or unsafe input handling.

## Scope

Security-sensitive areas include:

- HAL driver installation under `/Library/Audio/Plug-Ins/HAL`
- launch agent installation under `~/Library/LaunchAgents`
- shared ring files under `/tmp`
- helper logs under `~/Library/Logs/RocksmithMotuBridge`
- config loading from `~/Library/Application Support/RocksmithMotuBridge/config.plist`
- shell scripts that run install or uninstall actions

## Baseline Expectations

- The HAL plug-in must not open hardware devices or call Core Audio client APIs.
- Shared-memory/file-backed IPC must fail safely to silence.
- Shared ring files must reject symlinks, unsafe modes, unexpected sizes, and stale invalid headers before mapping.
- CLI input must be validated before mutating config or aggregate devices.
- Install scripts should avoid broad destructive filesystem operations.
