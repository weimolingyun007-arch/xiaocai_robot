#!/usr/bin/env python3
"""Evaluate the trained lightweight intent and correction classifiers."""

import argparse
import json
import sys
from pathlib import Path

from train_intent_classifier import (
    EvaluateCorrection,
    EvaluateIntent,
    LoadLabels,
    LoadRows,
)


def Main():
    script_dir = Path(__file__).parent
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--data", type=Path, default=script_dir / "data" / "intent_examples.jsonl"
    )
    parser.add_argument(
        "--correction-data",
        type=Path,
        default=script_dir / "data" / "intent_correction_examples.jsonl",
    )
    parser.add_argument(
        "--labels", type=Path, default=script_dir / "intent_labels.json"
    )
    parser.add_argument(
        "--model", type=Path, default=script_dir.parent / "assets" / "intent_model.json"
    )
    parser.add_argument(
        "--split", choices=("validation", "test"), default="test"
    )
    args = parser.parse_args()

    if not args.model.is_file():
        print(f"Model not found: {args.model}", file=sys.stderr)
        return 1
    model = json.loads(args.model.read_text(encoding="utf-8"))
    labels = LoadLabels(args.labels)
    rows = LoadRows([args.data, args.correction_data], labels)
    intent_metrics = EvaluateIntent(
        rows, model["intent_weights"], labels, args.split
    )
    correction_metrics = EvaluateCorrection(
        rows, model["correction_weights"], args.split
    )
    print(
        json.dumps(
            {
                "split": args.split,
                "intent_accuracy": intent_metrics["accuracy"],
                "intent_macro_f1": intent_metrics["macro_f1"],
                "intent_confusion_matrix": intent_metrics["confusion_matrix"],
                "correction": correction_metrics,
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())
