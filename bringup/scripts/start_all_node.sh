#!/usr/bin/env bash
set -eo pipefail

workspace=/home/lckfb/xiaocai_robot
if [ "$(id -u)" -eq 0 ]; then
  exec su -l lckfb -c "cd $workspace && bash bringup/scripts/start_all_node.sh"
fi

cd "$workspace"
export HOME="${HOME:-/home/lckfb}"
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}"
export ROS_LOG_DIR="${ROS_LOG_DIR:-/tmp/xiaocai_ros_logs}"
mkdir -p "$ROS_LOG_DIR"
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="$workspace/msgs:$workspace/thirdparty/onnxruntime:$workspace/thirdparty/sherpa-onnx:$workspace/thirdparty/vosk:$workspace/thirdparty:${LD_LIBRARY_PATH:-}"
export ALSA_CONFIG_PATH="$workspace/resource/audio/asound.conf"
export NODE_OPTIONS="${NODE_OPTIONS:+$NODE_OPTIONS }--require=$workspace/resource/music/ncm_player_compat.cjs"
export XIAOCAI_CONFIG_ROOT="$workspace/config"
export XIAOCAI_RESOURCE_ROOT="$workspace/resource"
source "$workspace/resource/deepseek.env"

log_dir="${XIAOCAI_LOG_DIR:-/tmp/xiaocai_logs}"
pid_file="/tmp/xiaocai_ros_nodes.pids"
mkdir -p "$log_dir"
bash bringup/scripts/stop_all_node.sh

pids=()
Cleanup() {
  for pid in "${pids[@]}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -f "$pid_file"
}
trap Cleanup EXIT
trap 'exit 0' INT TERM

export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"

if command -v xrandr >/dev/null 2>&1; then
  xrandr --output DSI-1 --rotate right
fi
if command -v xinput >/dev/null 2>&1; then
  input_devices="$(xinput list --short 2>/dev/null || true)"
  if grep -Fq 'my touch screen' <<< "$input_devices"; then
    xinput map-to-output 'my touch screen' DSI-1
  fi
fi

for module in audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node display_node; do
  executable="$workspace/bin/$module"
  if [ "$module" = dialogue_node ]; then
    DEEPSEEK_API_KEY="$DEEPSEEK_API_KEY" "$executable" >"$log_dir/${module}.log" 2>&1 &
  else
    "$executable" >"$log_dir/${module}.log" 2>&1 &
  fi
  pids+=("$!")
  printf '%s\n' "$!" >> "$pid_file"
done

echo "Xiaocai nodes started: ${pids[*]}"
echo "Logs: $log_dir"
echo "Stop with: bash bringup/scripts/stop_all_node.sh"
wait -n
