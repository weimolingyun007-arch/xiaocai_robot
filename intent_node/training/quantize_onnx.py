#!/usr/bin/env python3
"""Dynamically quantize an exported intent classifier to signed int8 weights."""

import argparse
from pathlib import Path

from onnxruntime.quantization import QuantType, quantize_dynamic


def Main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_model", type=Path)
    parser.add_argument("output_model", type=Path)
    args = parser.parse_args()
    args.output_model.parent.mkdir(parents=True, exist_ok=True)
    quantize_dynamic(
        model_input=str(args.input_model),
        model_output=str(args.output_model),
        weight_type=QuantType.QInt8,
        op_types_to_quantize=["MatMul", "Gemm"],
    )
    print(f"Wrote dynamically quantized model to {args.output_model}")


if __name__ == "__main__":
    Main()
