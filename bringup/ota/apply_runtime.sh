#!/usr/bin/env bash
set -Eeuo pipefail

archive="$1"
expected_sha="$2"
stamp="$3"
parent=/home/lckfb
workspace="$parent/xiaocai_robot"
legacy_workspace="$parent/Documents/xiaocai_robot"
legacy_parent="$parent/Documents"
stage="$parent/.xiaocai_robot_runtime_stage_$stamp"
backup="$parent/.xiaocai_robot_runtime_backup_$stamp"
payload="$stage/xiaocai_robot"
service=xiaocai.service
unit_target="/etc/systemd/system/$service"
unit_previous="$stage/$service.previous"
source_workspace=""
source_restore_path=""
unit_existed=0
unit_replaced=0
stopped=0
replaced=0
committed=0

Rollback() {
  status=$?
  trap - EXIT
  set +e
  if [[ "$replaced" -eq 1 ]]; then
    systemctl stop "$service"
    rm -rf "$workspace"
    mv "$backup" "$source_restore_path"
  fi
  if [[ "$unit_replaced" -eq 1 ]]; then
    if [[ "$unit_existed" -eq 1 ]]; then
      install -o root -g root -m 0644 "$unit_previous" "$unit_target"
    else
      rm -f "$unit_target"
    fi
    systemctl daemon-reload
  fi
  if [[ "$stopped" -eq 1 ]]; then systemctl start "$service"; fi
  rm -rf "$stage"
  if [[ "$committed" -eq 0 ]]; then rm -f "$archive"; fi
  exit "$status"
}
trap Rollback EXIT

[[ "$(id -u)" -eq 0 ]] || { echo 'ADB shell must be root.' >&2; exit 1; }
if [[ -d "$workspace" && -d "$legacy_workspace" ]]; then
  echo "Both the new and legacy workspaces exist; refusing to guess which contains the active runtime: $workspace and $legacy_workspace" >&2
  exit 1
elif [[ -d "$workspace" ]]; then
  source_workspace="$workspace"
  source_restore_path="$workspace"
elif [[ -d "$legacy_workspace" ]]; then
  source_workspace="$legacy_workspace"
  source_restore_path="$legacy_workspace"
else
  latest_backup=""
  for candidate in "$parent"/.xiaocai_robot_runtime_backup_* "$legacy_parent"/.xiaocai_robot_runtime_backup_*; do
    [[ -d "$candidate" ]] || continue
    if [[ -z "$latest_backup" || "$candidate" -nt "$latest_backup" ]]; then latest_backup="$candidate"; fi
  done
  if [[ -z "$latest_backup" ]]; then
    echo "Neither the new nor legacy workspace nor a runtime backup exists: $workspace, $legacy_workspace" >&2
    exit 1
  fi
  source_workspace="$latest_backup"
  source_restore_path="$(awk -F= '/^WorkingDirectory=/{print $2; exit}' "$unit_target" 2>/dev/null || true)"
  case "$source_restore_path" in
    "$workspace"|"$legacy_workspace") ;;
    *) source_restore_path="$legacy_workspace" ;;
  esac
  echo "Workspace paths are missing; recovering runtime state from: $source_workspace"
fi
secret_source="$source_workspace/resource/deepseek.env"
[[ -f "$secret_source" ]] || secret_source="$source_workspace/dialogue_node/.secrets/deepseek.env"
if [[ ! -f "$secret_source" ]]; then
  echo 'Existing DeepSeek settings are missing; refusing to replace the workspace.' >&2
  exit 1
fi
database_source="$source_workspace/resource/memory.sqlite3"
[[ -f "$database_source" ]] || database_source="$source_workspace/memory_node/data/memory.sqlite3"
if [[ ! -f "$database_source" ]]; then
  echo 'Existing memory database is missing; refusing to replace the workspace.' >&2
  exit 1
fi
[[ -f "$source_workspace/bringup/scripts/start_all_node.sh" ]] || { echo 'Existing startup script is missing; refusing to replace the workspace.' >&2; exit 1; }
[[ ! -e "$stage" && ! -e "$backup" ]] || { echo "Staging path already exists: $stamp" >&2; exit 1; }
actual_sha="$(sha256sum "$archive" | awk '{print $1}')"
[[ "$actual_sha" == "$expected_sha" ]] || { echo 'Package checksum mismatch.' >&2; exit 1; }
tar -tzf "$archive" | awk '
  $0 !~ /^xiaocai_robot(\/|$)/ || $0 ~ /(^|\/)\.\.(\/|$)/ || $0 ~ /^\// { bad=1 }
  END { if (NR == 0 || bad) exit 1 }
' || { echo 'Package contains an unexpected path.' >&2; exit 1; }
tar -tvzf "$archive" | awk 'substr($0, 1, 1) != "-" && substr($0, 1, 1) != "d" { bad=1 } END { if (bad) exit 1 }' || {
  echo 'Package contains a non-regular file.' >&2
  exit 1
}

mkdir -p "$stage"
tar -xzf "$archive" -C "$stage" --no-same-owner
[[ -z "$(find "$payload" -type l -print -quit)" ]] || { echo 'Package contains a symbolic link.' >&2; exit 1; }

modules=(audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node display_node)
for module in "${modules[@]}"; do
  [[ -f "$payload/config/$module/biz_config.toml" ]] || { echo "Package is missing config: $module/biz_config.toml" >&2; exit 1; }
done
total_modules=${#modules[@]}
[[ -x /usr/local/bin/ncm-cli && -x /usr/bin/mpv ]] || { echo "NetEase CLI or mpv is missing; install music dependencies first." >&2; exit 1; }
for module in "${modules[@]}"; do
  executable="$payload/bin/$module"
  [[ -x "$executable" ]] || { echo "Package is missing executable: $module" >&2; exit 1; }
  missing="$(LD_LIBRARY_PATH="$payload/msgs:$payload/thirdparty:/opt/ros/humble/lib:/usr/lib/aarch64-linux-gnu" ldd "$executable" 2>&1 | grep 'not found' || true)"
  [[ -z "$missing" ]] || { printf 'Unresolved libraries for %s:\n%s\n' "$module" "$missing" >&2; exit 1; }
done
for required in \
  resource/speech/wake_ack.wav \
  resource/audio/asound.conf \
  resource/music/ncm_player_compat.cjs \
  resource/display/idle.ppm \
  resource/display/listening.ppm \
  resource/display/thinking.ppm \
  resource/display/speaking.ppm \
  resource/display/quiet.ppm \
  resource/display/muted.ppm \
  resource/speech/silero_vad.onnx \
  resource/speech/vosk-model-small-cn-0.22/am/final.mdl \
  resource/speech/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/model.int8.onnx \
  resource/speech/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/tokens.txt \
  resource/tts/sherpa-onnx-vits-zh-ll/model.onnx \
  resource/tts/sherpa-onnx-vits-zh-ll/lexicon.txt \
  resource/tts/sherpa-onnx-vits-zh-ll/tokens.txt \
  resource/tts/sherpa-onnx-vits-zh-ll/phone.fst \
  resource/tts/sherpa-onnx-vits-zh-ll/date.fst \
  resource/tts/sherpa-onnx-vits-zh-ll/number.fst \
  bringup/scripts/start_all_node.sh \
  bringup/scripts/stop_all_node.sh \
  bringup/systemd/xiaocai.service; do
  [[ -f "$payload/$required" ]] || { echo "Package is missing: $required" >&2; exit 1; }
done
[[ -n "$(find "$payload/thirdparty" -maxdepth 1 -name '*.so' -print -quit)" ]] || {
  echo 'Package has no thirdparty shared libraries.' >&2
  exit 1
}
[[ -n "$(find "$payload/msgs" -maxdepth 1 -name 'libxiaocai_msgs*.so' -print -quit)" ]] || {
  echo 'Package has no generated xiaocai_msgs shared libraries.' >&2
  exit 1
}

systemctl stop "$service"
stopped=1
if [[ -f "$unit_target" ]]; then
  install -o root -g root -m 0644 "$unit_target" "$unit_previous"
  unit_existed=1
fi
install -o lckfb -g lckfb -m 0600 "$secret_source" "$payload/resource/deepseek.env"
for suffix in '' -wal -shm -journal; do
  source_file="$database_source$suffix"
  [[ -f "$source_file" ]] || continue
  install -o lckfb -g lckfb -m 0600 "$source_file" "$payload/resource/memory.sqlite3$suffix"
done
[[ -f "$payload/resource/deepseek.env" && -f "$payload/resource/memory.sqlite3" ]] || { echo 'Runtime state migration failed.' >&2; exit 1; }
chown -R lckfb:lckfb "$payload"
for module in "${modules[@]}"; do
  install -d -o lckfb -g lckfb -m 0755 "/userdata/log/$module"
done
mv "$source_workspace" "$backup"
replaced=1
mv "$payload" "$workspace"
install -o root -g root -m 0644 "$workspace/bringup/systemd/$service" "$unit_target"
unit_replaced=1
systemctl daemon-reload
systemctl start "$service"
for attempt in $(seq 1 40); do
  running=0
  for module in "${modules[@]}"; do
    if pgrep -f "^$workspace/bin/$module$" >/dev/null; then
      running=$((running + 1))
    fi
  done
  if [[ "$running" -eq "$total_modules" ]] && systemctl is-active --quiet "$service"; then break; fi
  sleep 1
done
if [[ "$running" -ne "$total_modules" ]] || ! systemctl is-active --quiet "$service"; then
  systemctl --no-pager --full status "$service" || true
  echo "Only $running of $total_modules nodes are running; restoring previous workspace." >&2
  exit 1
fi

committed=1
trap - EXIT
rm -rf "$stage"
rm -f "$archive"
for old_backup in "$parent"/.xiaocai_robot_runtime_backup_*; do
  [[ -d "$old_backup" && "$old_backup" != "$backup" ]] || continue
  rm -rf "$old_backup"
done
echo "Runtime workspace: $workspace"
echo "Previous workspace backup: $backup"
systemctl --no-pager --full status "$service"
