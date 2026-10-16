#!/usr/bin/env bash
set -eo pipefail

workspace=/home/lckfb/xiaocai_robot
if [ "$(id -u)" -eq 0 ]; then
  exec su -l lckfb -c "cd $workspace && bash bringup/scripts/stop_all_node.sh"
fi

declare -a pids=()
for proc_dir in /proc/[0-9]*; do
  pid=${proc_dir##*/}
  exe=$(readlink "$proc_dir/exe" 2>/dev/null || true)
  [ -n "$exe" ] || continue
  for module in audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node wifi_manager ble_manager display_node; do
    case "$exe" in
      *"/bin/$module"|*"/$module/bin/$module"*|*"/build/$module/$module"*)
        if kill -TERM "$pid" 2>/dev/null; then pids+=("$pid"); fi
        break
        ;;
    esac
  done
done

if [ "${#pids[@]}" -eq 0 ]; then
  echo "No Xiaocai nodes are running."
  exit 0
fi

for _ in {1..50}; do
  alive=0
  for pid in "${pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then alive=1; fi
  done
  [ "$alive" -eq 0 ] && break
  sleep 0.1
done

echo "Stop signal sent to Xiaocai node PIDs: ${pids[*]}"
