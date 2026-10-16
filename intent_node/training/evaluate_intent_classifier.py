#!/usr/bin/env python3
"""Report per-intent quality and review low-confidence held-out examples."""

import argparse
import json
from pathlib import Path

import numpy as np
import torch
from sklearn.metrics import classification_report, confusion_matrix
from transformers import AutoModelForSequenceClassification, AutoTokenizer

from train_intent_classifier import LoadLabels, LoadRows


def Main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--data",
        type=Path,
        default=Path(__file__).parent / "data" / "intent_examples.jsonl",
    )
    parser.add_argument(
        "--labels", type=Path, default=Path(__file__).parent / "intent_labels.json"
    )
    parser.add_argument(
        "--model-dir", type=Path, default=Path(__file__).parent / "output"
    )
    parser.add_argument("--split", choices=("validation", "test"), default="test")
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--max-length", type=int, default=96)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()

    labels = LoadLabels(args.labels)
    rows = [row for row in LoadRows(args.data, labels) if row["split"] == args.split]
    label2id = {label: index for index, label in enumerate(labels)}
    tokenizer = AutoTokenizer.from_pretrained(args.model_dir)
    model = AutoModelForSequenceClassification.from_pretrained(args.model_dir)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model.to(device)
    model.eval()

    predictions = []
    for offset in range(0, len(rows), args.batch_size):
        batch_rows = rows[offset : offset + args.batch_size]
        inputs = tokenizer(
            [row["text"] for row in batch_rows],
            padding=True,
            truncation=True,
            max_length=args.max_length,
            return_tensors="pt",
        ).to(device)
        with torch.inference_mode():
            probabilities = model(**inputs).logits.softmax(dim=-1).cpu().numpy()
        predictions.extend(probabilities)

    probabilities = np.asarray(predictions)
    predicted_ids = probabilities.argmax(axis=1)
    true_ids = np.asarray([label2id[row["label"]] for row in rows])
    report = classification_report(
        true_ids,
        predicted_ids,
        labels=list(range(len(labels))),
        target_names=labels,
        output_dict=True,
        zero_division=0,
    )
    matrix = confusion_matrix(
        true_ids, predicted_ids, labels=list(range(len(labels)))
    ).tolist()
    details = {
        "split": args.split,
        "sample_count": len(rows),
        "labels": labels,
        "classification_report": report,
        "confusion_matrix": matrix,
        "confusion_matrix_label_order": labels,
        "predictions": [
            {
                "text": row["text"],
                "expected": row["label"],
                "predicted": labels[int(predicted_id)],
                "confidence": float(probabilities[index, predicted_id]),
                "top_two_margin": float(
                    np.sort(probabilities[index])[-1]
                    - np.sort(probabilities[index])[-2]
                ),
            }
            for index, (row, predicted_id) in enumerate(zip(rows, predicted_ids))
        ],
    }
    serialized = json.dumps(details, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized + "\n", encoding="utf-8")
    print(serialized)


if __name__ == "__main__":
    Main()
