#!/bin/zsh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
helper_path="$repo_root/build/bin/RocksmithBridgeHelper"
plist_target="$HOME/Library/LaunchAgents/com.vhusso.rocksmithbridge.helper.plist"

if [[ ! -x "$helper_path" ]]; then
  echo "Build the helper first with: make helper" >&2
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"
sed "s#__HELPER_PATH__#$helper_path#g" "$repo_root/packaging/com.vhusso.rocksmithbridge.helper.plist" > "$plist_target"
launchctl unload "$plist_target" 2>/dev/null || true
launchctl load "$plist_target"
echo "Loaded launch agent: $plist_target"
