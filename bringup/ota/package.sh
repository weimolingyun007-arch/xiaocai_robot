#!/usr/bin/env bash
set -euo pipefail

script_path="$(readlink -f "${BASH_SOURCE[0]}")"
workspace="$(cd "$(dirname "$script_path")/../.." && pwd)"

binary_root="$workspace/build"

if ! command -v readelf >/dev/null 2>&1; then
  echo "readelf is required to verify AArch64 executables." >&2
  exit 127
fi

temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/xiaocai-package.XXXXXX")"
temp_archive=""
cleanup() {
  if [[ -n "$temp_archive" ]]; then
    rm -f "$temp_archive"
  fi
  rm -rf "$temp_dir"
}
trap cleanup EXIT

payload="$temp_dir/xiaocai_robot"
mkdir -p "$payload/bin" "$payload/config" "$payload/resource" "$payload/thirdparty" "$payload/msgs" "$payload/bringup/scripts" "$payload/bringup/systemd"
install -D -m 0644 "$workspace/audio_io_node/assets/asound.conf" "$payload/resource/audio/asound.conf"

modules=(audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node)
for module in "${modules[@]}"; do
  install -D -m 0644 "$workspace/$module/config/biz_config.toml" "$payload/config/$module/biz_config.toml"
done
for module in "${modules[@]}"; do
  binary="$binary_root/$module/$module"
  if [[ ! -x "$binary" ]]; then
    echo "Built executable not found: $binary. Run 'make build' first." >&2
    exit 1
  fi
  machine="$(readelf -h "$binary" | awk -F: '/Machine:/{gsub(/^ +/,"",$2); print $2}')"
  if [[ "$machine" != AArch64 ]]; then
    echo "Expected an AArch64 executable, got '$machine': $binary" >&2
    exit 1
  fi
  install -D -m 0755 "$binary" "$payload/bin/$module"
done

runtime_libraries=(
  "$workspace/thirdparty/onnxruntime/libonnxruntime.so"
  "$workspace/thirdparty/sherpa-onnx/libsherpa-onnx-c-api.so"
  "$workspace/thirdparty/vosk/libvosk.so"
)
library_count=0
for library in "${runtime_libraries[@]}"; do
  if [[ ! -f "$library" ]]; then
    echo "Runtime dependency library not found: $library" >&2
    exit 1
  fi
  machine="$(readelf -h "$library" | awk -F: '/Machine:/{gsub(/^ +/,"",$2); print $2}')"
  if [[ "$machine" != AArch64 ]]; then
    echo "Expected an AArch64 dependency library, got '$machine': $library" >&2
    exit 1
  fi
  install -D -m 0755 "$library" "$payload/thirdparty/$(basename "$library")"
  library_count=$((library_count + 1))
done
if [[ "$library_count" -ne 3 ]]; then
  echo "Expected all three runtime shared libraries, found $library_count." >&2
  exit 1
fi

interface_library_count=0
for library in "$binary_root/install/lib"/libxiaocai_msgs*.so; do
  [[ -f "$library" ]] || continue
  machine="$(readelf -h "$library" | awk -F: '/Machine:/{gsub(/^ +/,"",$2); print $2}')"
  if [[ "$machine" != AArch64 ]]; then
    echo "Expected an AArch64 interface library, got '$machine': $library" >&2
    exit 1
  fi
  install -D -m 0755 "$library" "$payload/msgs/$(basename "$library")"
  interface_library_count=$((interface_library_count + 1))
done
if [[ "$interface_library_count" -eq 0 ]]; then
  echo "Generated xiaocai_msgs runtime libraries not found in $binary_root/install/lib. Run 'make build' first." >&2
  exit 1
fi

speech_assets="$workspace/speech_input_node/assets"
speech_payload="$payload/resource/speech"
vosk_model=vosk-model-small-cn-0.22
sense_model=sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17
install -D -m 0644 "$speech_assets/wake_ack.wav" "$speech_payload/wake_ack.wav"
install -D -m 0644 "$speech_assets/wake_ack_ding.wav" "$speech_payload/wake_ack_ding.wav"
install -D -m 0644 "$speech_assets/silero_vad.onnx" "$speech_payload/silero_vad.onnx"
for asset in \
  am/final.mdl \
  conf/mfcc.conf \
  conf/model.conf \
  graph/Gr.fst \
  graph/HCLr.fst \
  graph/disambig_tid.int \
  graph/phones/word_boundary.int \
  ivector/final.dubm \
  ivector/final.ie \
  ivector/final.mat \
  ivector/global_cmvn.stats \
  ivector/online_cmvn.conf \
  ivector/splice.conf; do
  install -D -m 0644 "$speech_assets/$vosk_model/$asset" "$speech_payload/$vosk_model/$asset"
done
install -D -m 0644 "$speech_assets/$sense_model/model.int8.onnx" "$speech_payload/$sense_model/model.int8.onnx"
install -D -m 0644 "$speech_assets/$sense_model/tokens.txt" "$speech_payload/$sense_model/tokens.txt"

tts_assets="$workspace/tts_node/assets/sherpa-onnx-vits-zh-ll"
tts_payload="$payload/resource/tts/sherpa-onnx-vits-zh-ll"
for asset in model.onnx lexicon.txt tokens.txt phone.fst date.fst number.fst; do
  install -D -m 0644 "$tts_assets/$asset" "$tts_payload/$asset"
done

for script in start_all_node.sh stop_all_node.sh; do
  install -D -m 0755 "$workspace/bringup/scripts/$script" "$payload/bringup/scripts/$script"
done
install -D -m 0644 "$workspace/bringup/ota/xiaocai.service" "$payload/bringup/systemd/xiaocai.service"

package_name="xiaocai_robot_$(date +%Y%m%d_%H%M%S).tar.gz"
archive="$workspace/$package_name"
temp_archive="$archive.tmp.$$"
tar -C "$temp_dir" -czf "$temp_archive" xiaocai_robot
gzip -t "$temp_archive"

if ! tar -tzf "$temp_archive" | awk '
BEGIN {
  split("audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node", names, " ")
  for (i in names) node[names[i]]=1
}
{
  path=$0
  sub(/\/$/, "", path)
  if (path == "xiaocai_robot" || path == "xiaocai_robot/bin" || path == "xiaocai_robot/config" || path == "xiaocai_robot/resource" || path == "xiaocai_robot/resource/speech" || path == "xiaocai_robot/resource/tts" || path == "xiaocai_robot/resource/audio" || path == "xiaocai_robot/thirdparty" || path == "xiaocai_robot/msgs" || path == "xiaocai_robot/bringup" || path == "xiaocai_robot/bringup/scripts" || path == "xiaocai_robot/bringup/systemd") next
  count=split(path, part, "/")
  if (count == 3 && part[1] == "xiaocai_robot" && part[2] == "config" && (part[3] in node)) next
  if (count == 4 && part[1] == "xiaocai_robot" && part[2] == "config" && (part[3] in node) && part[4] == "biz_config.toml") { configs[part[3]]++; next }
  if (count == 3 && part[1] == "xiaocai_robot" && part[2] == "bin" && (part[3] in node)) {
    executable[part[3]]++
    next
  }
  if (count == 3 && part[1] == "xiaocai_robot" && part[2] == "thirdparty" && part[3] ~ /\.so$/) { libraries++; next }
  if (count == 3 && part[1] == "xiaocai_robot" && part[2] == "msgs" && part[3] ~ /^libxiaocai_msgs.*\.so$/) { interface_libraries++; next }
  if (path == "xiaocai_robot/bringup/scripts/start_all_node.sh") { start_script++; next }
  if (path == "xiaocai_robot/bringup/scripts/stop_all_node.sh") { stop_script++; next }
  if (path == "xiaocai_robot/bringup/systemd/xiaocai.service") { service_unit++; next }
  if (count >= 4 && part[1] == "xiaocai_robot" && part[2] == "resource" && (part[3] == "speech" || part[3] == "tts")) { assets++; next }
  if (path == "xiaocai_robot/resource/audio/asound.conf") { alsa_config++; next }
  print "Unexpected package entry: " path > "/dev/stderr"
  invalid=1
}
END {
  for (name in node) if (executable[name] != 1) { print "Missing executable: " name > "/dev/stderr"; invalid=1 }
  for (name in node) if (configs[name] != 1) { print "Missing node config: " name > "/dev/stderr"; invalid=1 }
  if (libraries == 0 || interface_libraries == 0 || assets == 0 || alsa_config != 1 || start_script != 1 || stop_script != 1 || service_unit != 1) { print "Missing runtime dependency, custom message library, asset, ALSA configuration, bringup script, or systemd unit" > "/dev/stderr"; invalid=1 }
  if (invalid) exit 1
}'; then
  echo "Package must contain only node executables, runtime libraries, model assets, bringup scripts, and the systemd unit." >&2
  exit 1
fi

mv "$temp_archive" "$archive"
temp_archive=""
for old_archive in "$workspace"/xiaocai_robot_*.tar.gz; do
  [[ -f "$old_archive" ]] || continue
  [[ "$old_archive" == "$archive" ]] && continue
  rm -f -- "$old_archive"
done

echo "Package created: $archive"
du -h "$archive"
sha256sum "$archive"
