#!/bin/zsh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
installed_helper="/usr/local/libexec/RocksmithMotuBridge/RocksmithBridgeHelper"
plist_target="$HOME/Library/LaunchAgents/com.vhusso.rocksmithbridge.helper.plist"
log_dir="$HOME/Library/Logs/RocksmithMotuBridge"
stdout_log="$log_dir/helper.out"
stderr_log="$log_dir/helper.err"
domain="gui/$(id -u)"

if [[ ! -x "$installed_helper" ]]; then
  echo "Install first with: sudo make install-local" >&2
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"
mkdir -p "$log_dir"
chmod 700 "$log_dir"
sed \
  -e "s#__HELPER_PATH__#$installed_helper#g" \
  -e "s#__HELPER_ERROR_LOG__#$stderr_log#g" \
  -e "s#__HELPER_OUTPUT_LOG__#$stdout_log#g" \
  "$repo_root/packaging/com.vhusso.rocksmithbridge.helper.plist" > "$plist_target"
launchctl bootout "$domain" "$plist_target" 2>/dev/null || true
launchctl bootstrap "$domain" "$plist_target"
launchctl kickstart -k "$domain/com.vhusso.rocksmithbridge.helper"
echo "Loaded launch agent: $plist_target"
