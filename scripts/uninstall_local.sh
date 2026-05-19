#!/bin/zsh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
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
trash_dir="$target_home/.Trash/RocksmithMotuBridge uninstall $(date +%Y%m%d%H%M%S)"
ctl="$repo_root/build/bin/rocksmith_bridge_ctl"

remove_path() {
  local path="$1"
  case "$path" in
    "$agent"|"$driver"|"$helper_dir") ;;
    *)
      echo "Refusing to remove unexpected path: $path" >&2
      exit 1
      ;;
  esac
  mkdir -p "$trash_dir"
  /bin/mv "$path" "$trash_dir/$(basename "$path")"
  chown -R "$target_user" "$trash_dir" 2>/dev/null || true
}

if [[ -x "$ctl" ]]; then
  if [[ "$(id -u)" == "0" && "$target_user" != "root" ]]; then
    /usr/bin/sudo -u "$target_user" "$ctl" destroy-aggregate 2>/dev/null || true
  else
    "$ctl" destroy-aggregate 2>/dev/null || true
  fi
else
  echo "Aggregate cleanup skipped; build rocksmith_bridge_ctl first to remove aggregate devices automatically." >&2
fi

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
echo "Moved Rocksmith MOTU Bridge local install to: $trash_dir"
