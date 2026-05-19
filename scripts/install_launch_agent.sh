#!/bin/zsh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
installed_helper="/usr/local/libexec/RocksmithMotuBridge/RocksmithBridgeHelper"
helper_path="$installed_helper"
plist_target="$HOME/Library/LaunchAgents/com.vhusso.rocksmithbridge.helper.plist"
domain="gui/$(id -u)"

if [[ ! -x "$helper_path" ]]; then
  helper_path="$repo_root/build/bin/RocksmithBridgeHelper"
fi

if [[ ! -x "$helper_path" ]]; then
  echo "Install first with: sudo make install-local" >&2
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"
sed "s#__HELPER_PATH__#$helper_path#g" "$repo_root/packaging/com.vhusso.rocksmithbridge.helper.plist" > "$plist_target"
launchctl bootout "$domain" "$plist_target" 2>/dev/null || true
launchctl bootstrap "$domain" "$plist_target"
launchctl kickstart -k "$domain/com.vhusso.rocksmithbridge.helper"
echo "Loaded launch agent: $plist_target"
