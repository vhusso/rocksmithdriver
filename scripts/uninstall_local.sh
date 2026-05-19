#!/bin/zsh
set -euo pipefail

driver="/Library/Audio/Plug-Ins/HAL/RocksmithMotuBridge.driver"
agent="$HOME/Library/LaunchAgents/com.vhusso.rocksmithbridge.helper.plist"

if [[ -f "$agent" ]]; then
  launchctl unload "$agent" 2>/dev/null || true
  trash "$agent"
fi

if [[ -d "$driver" ]]; then
  trash "$driver"
fi

killall coreaudiod 2>/dev/null || true
echo "Removed Rocksmith MOTU Bridge local install."
