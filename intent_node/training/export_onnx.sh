#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
model_dir="${1:-${script_dir}/output}"
output_dir="${2:-${script_dir}/onnx}"

optimum-cli export onnx \
  --model "${model_dir}" \
  --task text-classification \
  "${output_dir}"
