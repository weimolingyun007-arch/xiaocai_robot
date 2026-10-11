#!/usr/bin/env bash
set -euo pipefail

script_path="$(readlink -f "$0")"
workspace="$(cd "$(dirname "$script_path")/../.." && pwd)"

if [[ $# -gt 1 ]]; then
  echo "Usage: bash bringup/ota/deploy_adb.sh [package.tar.gz]" >&2
  exit 2
fi

if [[ $# -eq 1 ]]; then
  archive="$(readlink -f "$1")"
else
  shopt -s nullglob
  archive=""
  for candidate in "$workspace"/xiaocai_robot_*.tar.gz; do archive="$candidate"; done
  if [[ -z "$archive" ]]; then
    echo "No package found. Run make ota first." >&2
    exit 1
  fi
fi

if [[ ! -f "$archive" ]]; then
  echo "Package not found: $archive" >&2
  exit 1
fi

gzip -t "$archive"
if ! tar -tzf "$archive" | awk '
  $0 !~ /^xiaocai_robot(\/|$)/ || $0 ~ /(^|\/)\.\.(\/|$)/ || $0 ~ /^\// { bad=1 }
  END { if (NR == 0 || bad) exit 1 }
'; then
  echo "Package contains an unexpected path." >&2
  exit 1
fi
if tar -tzf "$archive" | awk '/(^|\/)(src|include|CMakeLists\.txt|package\.xml|README[^/]*|test_wavs|export-onnx\.py)(\/|$)|\.(cpp|hpp|h|py)$|^xiaocai_robot\/bringup\/ota(\/|$)/ { found=1 } END { exit !found }'; then
  echo "Package contains source or build files; refusing deployment." >&2
  exit 1
fi
if ! tar -tvzf "$archive" | awk 'substr($0, 1, 1) != "-" && substr($0, 1, 1) != "d" { bad=1 } END { if (bad) exit 1 }'; then
  echo "Package contains a non-regular file; refusing deployment." >&2
  exit 1
fi

if ! command -v adb >/dev/null 2>&1; then
  echo "adb is required to deploy the package." >&2
  exit 127
fi
serial="$(printenv ANDROID_SERIAL 2>/dev/null || true)"
RunAdb() {
  if [[ -n "$serial" ]]; then command adb -s "$serial" "$@"; else command adb "$@"; fi
}
if [[ "$(RunAdb get-state 2>/dev/null | tr -d '\r')" != device ]]; then
  echo "No ready ADB device found. Check the board connection." >&2
  exit 1
fi

remote_arch="$(RunAdb shell uname -m | tr -d '\r')"
if [[ "$remote_arch" != aarch64 && "$remote_arch" != arm64 ]]; then
  echo "Connected device is not AArch64: $remote_arch" >&2
  exit 1
fi
remote_uid="$(RunAdb shell id -u | tr -d '\r')"
if [[ "$remote_uid" != 0 ]]; then
  echo "ADB shell must have root access to migrate and update the runtime under /home/lckfb/xiaocai_robot." >&2
  exit 1
fi

remote_archive=/home/lckfb/.xiaocai_robot_runtime.tar.gz
remote_script=/home/lckfb/.xiaocai_robot_runtime_deploy.sh
expected_sha="$(sha256sum "$archive" | awk '{print $1}')"
stamp="$(date +%Y%m%d_%H%M%S)_$$"
remote_sha="$(RunAdb shell sha256sum "$remote_archive" 2>/dev/null | awk '{print $1}' | tr -d '\r' || true)"
if [[ "$remote_sha" != "$expected_sha" ]]; then
  RunAdb push "$archive" "$remote_archive"
fi

local_script="$workspace/bringup/ota/apply_runtime.sh"

RunAdb push "$local_script" "$remote_script"
marker="__XIAOCAI_DEPLOY_STATUS_${stamp}__"
if ! remote_output="$(RunAdb shell "bash '$remote_script' '$remote_archive' '$expected_sha' '$stamp'; status=\$?; rm -f '$remote_script'; printf '${marker}%s\n' \"\$status\"; exit \$status" 2>&1)"; then
  printf '%s\n' "$remote_output" >&2
  exit 1
fi
printf '%s\n' "$remote_output" | sed "/${marker}/d"
if ! printf '%s\n' "$remote_output" | tr -d '\r' | grep -Fxq "${marker}0"; then
  echo "Board deployment failed; no success status returned." >&2
  exit 1
fi

echo "Deployed runtime package: $archive"
echo "Runtime workspace: /home/lckfb/xiaocai_robot"
echo "If a board shell was already inside the old workspace, run: cd /home/lckfb/xiaocai_robot"
