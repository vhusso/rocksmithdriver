#!/bin/zsh
set -euo pipefail

driver="/Library/Audio/Plug-Ins/HAL/RocksmithMotuBridge.driver"
helper_dir="/usr/local/libexec/RocksmithMotuBridge"
target_user="${SUDO_USER:-$(id -un)}"
target_uid="$(id -u "$target_user")"
target_home="$(dscl . -read "/Users/$target_user" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
if [[ -z "$target_home" ]]; then
  target_home="$HOME"
fi
agent="$target_home/Library/LaunchAgents/com.vhusso.rocksmithbridge.helper.plist"
domain="gui/$target_uid"

remove_path() {
  local path="$1"
  if command -v trash >/dev/null 2>&1; then
    trash "$path"
  else
    echo "Install trash first or remove manually: $path" >&2
    exit 1
  fi
}

if [[ -f "$agent" ]]; then
  launchctl bootout "$domain" "$agent" 2>/dev/null || true
  remove_path "$agent"
fi

if [[ -d "$driver" ]]; then
  remove_path "$driver"
fi

if [[ -d "$helper_dir" ]]; then
  remove_path "$helper_dir"
fi

killall coreaudiod 2>/dev/null || true
echo "Removed Rocksmith MOTU Bridge local install."
