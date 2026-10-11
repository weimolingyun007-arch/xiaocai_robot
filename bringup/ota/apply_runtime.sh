#!/usr/bin/env bash
set -Eeuo pipefail

archive="$1"
expected_sha="$2"
stamp="$3"
parent=/home/lckfb/Documents
workspace="$parent/xiaocai_robot"
stage="$parent/.xiaocai_robot_runtime_stage_$stamp"
backup="$parent/.xiaocai_robot_runtime_backup_$stamp"
payload="$stage/xiaocai_robot"
service=xiaocai.service
stopped=0
replaced=0
committed=0

rollback() {
  status=$?
  trap - EXIT
  set +e
  if [[ "$replaced" -eq 1 ]]; then
    systemctl stop "$service"
    rm -rf "$workspace"
    mv "$backup" "$workspace"
  fi
  if [[ "$stopped" -eq 1 ]]; then systemctl start "$service"; fi
  rm -rf "$stage"
  if [[ "$committed" -eq 0 ]]; then rm -f "$archive"; fi
  exit "$status"
}
trap rollback EXIT

[[ "$(id -u)" -eq 0 ]] || { echo 'ADB shell must be root.' >&2; exit 1; }
[[ -d "$workspace" ]] || { echo "Existing workspace is missing: $workspace" >&2; exit 1; }
secret_source="$workspace/resource/deepseek.env"
[[ -f "$secret_source" ]] || secret_source="$workspace/dialogue_node/.secrets/deepseek.env"
if [[ ! -f "$secret_source" ]]; then
  echo 'Existing DeepSeek settings are missing; refusing to replace the workspace.' >&2
  exit 1
fi
database_source="$workspace/resource/memory.sqlite3"
[[ -f "$database_source" ]] || database_source="$workspace/memory_node/data/memory.sqlite3"
if [[ ! -f "$database_source" ]]; then
  echo 'Existing memory database is missing; refusing to replace the workspace.' >&2
  exit 1
fi
[[ -f "$workspace/bringup/scripts/start_all_node.sh" ]] || { echo 'Existing startup script is missing; refusing to replace the workspace.' >&2; exit 1; }
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

modules=(audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node)
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
  resource/speech/wake_ack_ding.wav \
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
  bringup/scripts/stop_all_node.sh; do
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
install -o lckfb -g lckfb -m 0600 "$secret_source" "$payload/resource/deepseek.env"
for suffix in '' -wal -shm -journal; do
  source_file="$database_source$suffix"
  [[ -f "$source_file" ]] || continue
  install -o lckfb -g lckfb -m 0600 "$source_file" "$payload/resource/memory.sqlite3$suffix"
done
[[ -f "$payload/resource/deepseek.env" && -f "$payload/resource/memory.sqlite3" ]] || { echo 'Runtime state migration failed.' >&2; exit 1; }
chown -R lckfb:lckfb "$payload"
mv "$workspace" "$backup"
replaced=1
mv "$payload" "$workspace"
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
