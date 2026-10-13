#!/usr/bin/env bash
set -euo pipefail

script_path="$(readlink -f "${BASH_SOURCE[0]}")"
workspace="$(cd "$(dirname "$script_path")/../.." && pwd)"
machine="$(uname -m)"
build_root="$workspace/build"
interface_prefix="$build_root/install"

case "$machine" in
  aarch64|arm64)
    build_mode=native
    build_jobs="${XIAOCAI_BUILD_JOBS:-1}"
    if [[ "$(id -u)" -eq 0 ]] && getent passwd lckfb >/dev/null; then
      exec su -l lckfb -c "bash '$script_path'"
    fi
    ;;
  x86_64|amd64)
    build_mode=cross
    build_jobs="${XIAOCAI_BUILD_JOBS:-2}"
    toolchain_root="${XIAOCAI_TOOLCHAIN_ROOT:-/opt/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu}"
    sysroot="${XIAOCAI_SYSROOT:-${HOME}/.cache/xiaocai_robot/aarch64-sysroot}"
    toolchain_file="$workspace/bringup/ota/aarch64-toolchain.cmake"

    if [[ ! -x "$toolchain_root/bin/aarch64-none-linux-gnu-g++" ]]; then
      echo "AArch64 cross compiler not found: $toolchain_root/bin/aarch64-none-linux-gnu-g++" >&2
      exit 1
    fi
    if [[ ! -d "$sysroot/opt/ros/humble/share/ament_cmake/cmake" ]]; then
      echo "AArch64 ROS sysroot not found: $sysroot" >&2
      echo "Set XIAOCAI_SYSROOT to the sysroot containing opt/ros/humble and usr/lib/aarch64-linux-gnu." >&2
      exit 1
    fi

    export AMENT_PREFIX_PATH="$interface_prefix:$sysroot/opt/ros/humble"
    export CMAKE_PREFIX_PATH="$interface_prefix:$sysroot/opt/ros/humble"
    export PYTHONPATH="$sysroot/opt/ros/humble/lib/python3.10/site-packages:$sysroot/opt/ros/humble/local/lib/python3.10/dist-packages:$sysroot/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}"
    ;;
  *)
    echo "Unsupported build host architecture: $machine (expected x86_64 or aarch64)." >&2
    exit 1
    ;;
esac

if ! command -v cmake >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
  echo "cmake and make are required to build the robot nodes." >&2
  exit 127
fi

if [[ "$build_mode" == native ]]; then
  if [[ ! -f /opt/ros/humble/setup.bash ]]; then
    echo "ROS 2 Humble setup file not found: /opt/ros/humble/setup.bash" >&2
    exit 1
  fi
  # shellcheck disable=SC1091
  source /opt/ros/humble/setup.bash
  echo "Build mode: native on $machine"
  export AMENT_PREFIX_PATH="$interface_prefix${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
  export CMAKE_PREFIX_PATH="$interface_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
else
  echo "Build mode: cross-compile x86_64 -> AArch64"
fi

export XIAOCAI_INTERFACE_PREFIX="$interface_prefix"

modules=(audio_io_node memory_node intent_node dialogue_node tts_node speech_input_node music_node display_node)
total_modules="${#modules[@]}"
completed_modules=0
log_file="$(mktemp "${TMPDIR:-/tmp}/xiaocai-build.XXXXXX")"
trap 'rm -f "$log_file"' EXIT

printf '[  0%%] Build started (0/%d nodes complete)\n' "$total_modules"
interface_build_dir="$build_root/xiaocai_msgs"
interface_cmake_args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$interface_prefix")
if [[ "$build_mode" == cross ]]; then
  interface_cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$toolchain_file")
  interface_cmake_args+=(-DXIAOCAI_TOOLCHAIN_ROOT="$toolchain_root")
  interface_cmake_args+=(-DXIAOCAI_SYSROOT="$sysroot")
fi
printf 'Building xiaocai_msgs custom interfaces\n'
if ! cmake -S "$workspace/xiaocai_msgs" -B "$interface_build_dir" "${interface_cmake_args[@]}" >"$log_file" 2>&1; then
  cat "$log_file" >&2
  echo "CMake configuration failed for xiaocai_msgs." >&2
  exit 1
fi
if ! make -C "$interface_build_dir" "-j$build_jobs" >>"$log_file" 2>&1; then
  cat "$log_file" >&2
  echo "Compilation failed for xiaocai_msgs." >&2
  exit 1
fi
if ! make -C "$interface_build_dir" install >>"$log_file" 2>&1; then
  cat "$log_file" >&2
  echo "Installation failed for xiaocai_msgs." >&2
  exit 1
fi

for module in "${modules[@]}"; do
  source_dir="$workspace/$module"
  build_dir="$build_root/$module"
  mkdir -p "$build_dir"

  if [[ "$build_mode" == cross ]]; then
    system_file="$(find "$build_dir/CMakeFiles" -name CMakeSystem.cmake -print -quit 2>/dev/null || true)"
    if [[ -n "$system_file" ]] && ! grep -Fq -- "include(\"$toolchain_file\")" "$system_file"; then
      echo "Resetting stale CMake configuration for $module (toolchain path changed)."
      rm -rf "$build_dir"
      mkdir -p "$build_dir"
    fi
  fi

  cmake_args=(-DCMAKE_BUILD_TYPE=Release)
  if [[ "$build_mode" == cross ]]; then
    cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$toolchain_file")
    cmake_args+=(-DXIAOCAI_TOOLCHAIN_ROOT="$toolchain_root")
    cmake_args+=(-DXIAOCAI_SYSROOT="$sysroot")
  fi
  cmake_args+=(-Dxiaocai_msgs_DIR="$interface_prefix/share/xiaocai_msgs/cmake")

  printf 'Building %s (%d/%d)\n' "$module" "$((completed_modules + 1))" "$total_modules"
  if ! cmake -S "$source_dir" -B "$build_dir" "${cmake_args[@]}" >"$log_file" 2>&1; then
    cat "$log_file" >&2
    echo "CMake configuration failed for $module." >&2
    exit 1
  fi
  if ! make -C "$build_dir" "-j$build_jobs" >>"$log_file" 2>&1; then
    cat "$log_file" >&2
    echo "Compilation failed for $module." >&2
    exit 1
  fi

  warnings="$(grep -E 'warning:' "$log_file" || true)"
  if [[ -n "$warnings" ]]; then
    printf '%s\n' "$warnings" >&2
  fi

  completed_modules=$((completed_modules + 1))
  progress=$((completed_modules * 100 / total_modules))
  printf '[%3d%%] %d/%d nodes complete (%s)\n' \
    "$progress" "$completed_modules" "$total_modules" "$module"
done

python3 "$workspace/bringup/ota/merge_compile_commands.py" "$build_root"

echo "All Xiaocai nodes compiled into $build_root."
