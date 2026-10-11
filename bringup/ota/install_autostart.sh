#!/usr/bin/env bash
set -euo pipefail

workspace=/home/lckfb/Documents/xiaocai_robot
unit_name=xiaocai.service
unit_source="$workspace/bringup/ota/$unit_name"
unit_target="/etc/systemd/system/$unit_name"

if [ "$(id -u)" -ne 0 ]; then
  exec sudo -E bash "$0" "$@"
fi

if [ ! -f "$unit_source" ]; then
  echo "Missing service file: $unit_source" >&2
  exit 1
fi
if [ ! -x "$workspace/bin/audio_io_node" ] && [ ! -x "$workspace/build/audio_io_node/audio_io_node" ]; then
  echo "ROS 2 nodes are not built yet; run bringup/ota/build.sh first." >&2
  exit 1
fi
if [ ! -r "$workspace/resource/deepseek.env" ]; then
  echo "Missing readable DeepSeek settings: $workspace/resource/deepseek.env" >&2
  exit 1
fi

install -o root -g root -m 0644 "$unit_source" "$unit_target"
systemctl daemon-reload
systemctl enable --now "$unit_name"
systemctl --no-pager --full status "$unit_name"
