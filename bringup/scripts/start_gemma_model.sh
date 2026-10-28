#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd -- "$script_dir/../.." && pwd)"
model_path="${GEMMA_MODEL_PATH:-$workspace/models/gemma4-E4B/gemma4-E4B_q4_0-it.gguf}"
model_alias="${GEMMA_MODEL_ALIAS:-gemma4-e4b}"
host="${GEMMA_HOST:-0.0.0.0}"
port="${GEMMA_PORT:-8081}"
context_size="${GEMMA_CONTEXT_SIZE:-8192}"
threads="${GEMMA_THREADS:-16}"
log_content=0

while (($# > 0)); do
  case "$1" in
    --log-content)
      log_content=1
      ;;
    -h|--help)
      echo "Usage: $0 [--log-content]"
      echo "  --log-content  Print full model request and response bodies (debug only)."
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: $0 [--log-content]" >&2
      exit 2
      ;;
  esac
  shift
done

verbosity_args=()
if ((log_content)); then
  verbosity_args=(--verbosity 5)
fi

if [[ -n "${LLAMA_SERVER_BIN:-}" ]]; then
  server_bin="$LLAMA_SERVER_BIN"
elif command -v llama-server >/dev/null 2>&1; then
  server_bin="$(command -v llama-server)"
elif [[ -x "$workspace/thirdparty/llama.cpp/build/bin/llama-server" ]]; then
  server_bin="$workspace/thirdparty/llama.cpp/build/bin/llama-server"
elif [[ -x "$workspace/build/llama.cpp/bin/llama-server" ]]; then
  server_bin="$workspace/build/llama.cpp/bin/llama-server"
else
  echo "llama-server not found. Install/build llama.cpp with server support, or set LLAMA_SERVER_BIN." >&2
  exit 127
fi

if [[ ! -x "$server_bin" ]]; then
  echo "LLAMA_SERVER_BIN is not executable: $server_bin" >&2
  exit 127
fi

if [[ ! -f "$model_path" ]]; then
  echo "Gemma model file not found: $model_path" >&2
  exit 1
fi

if ! [[ "$port" =~ ^[0-9]+$ ]] || ((port < 1 || port > 65535)); then
  echo "GEMMA_PORT must be between 1 and 65535: $port" >&2
  exit 2
fi

advertise_ip="${GEMMA_ADVERTISE_IP:-}"
if [[ -z "$advertise_ip" ]] && command -v ip >/dev/null 2>&1; then
  advertise_ip="$(ip -4 -o addr show scope global 2>/dev/null | awk \
    '$2 !~ /^(docker|br-|veth|Mihomo)/ {sub(/\/.*/, "", $4); print $4; exit}' || true)"
fi

echo "Starting Gemma 4 E4B Q4 model"
echo "Model: $model_path"
echo "Server: $server_bin"
echo "Listen: $host:$port"
if ((log_content)); then
  echo "Request/response content logging: enabled"
else
  echo "Request/response content logging: disabled"
fi
if [[ -n "$advertise_ip" ]]; then
  echo "Board endpoint: http://$advertise_ip:$port/v1/chat/completions"
fi
echo "Stop with Ctrl-C."

exec "$server_bin" \
  --model "$model_path" \
  --alias "$model_alias" \
  --host "$host" \
  --port "$port" \
  --ctx-size "$context_size" \
  --threads "$threads" \
  "${verbosity_args[@]}" \
  --jinja
