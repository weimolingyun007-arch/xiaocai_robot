#!/usr/bin/env python3
"""Train the small character n-gram intent model using only Python's stdlib."""

import argparse
import hashlib
import json
import math
import random
from pathlib import Path


SPLITS = {"train", "validation", "test"}
FEATURE_DIMENSIONS = 8192
MAX_NGRAM = 3
EPOCHS = 120
LEARNING_RATE = 0.18
L2 = 0.0001


def LoadLabels(path):
    payload = json.loads(path.read_text(encoding="utf-8"))
    labels = [entry["name"] for entry in payload["labels"]]
    if not labels or len(labels) != len(set(labels)):
        raise ValueError(f"Invalid or duplicate labels in {path}")
    return labels


def NormalizeText(text):
    return "".join(character for character in text if not character.isspace())


def FeatureIndex(ngram):
    value = 2166136261
    for byte in ngram.encode("utf-8"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value % FEATURE_DIMENSIONS


def FeatureVector(text):
    characters = list(NormalizeText(text))
    padded = ["^", *characters, "$"]
    counts = {}
    for length in range(1, MAX_NGRAM + 1):
        for start in range(len(padded) - length + 1):
            index = FeatureIndex("".join(padded[start : start + length]))
            counts[index] = counts.get(index, 0.0) + 1.0
    norm = math.sqrt(sum(value * value for value in counts.values())) or 1.0
    return [(index, value / norm) for index, value in counts.items()]


def LoadRows(paths, labels):
    rows = []
    normalized_splits = {}
    for path in paths:
        with path.open(encoding="utf-8") as data_file:
            for line_number, line in enumerate(data_file, start=1):
                if not line.strip():
                    continue
                row = json.loads(line)
                text = row.get("text", "").strip()
                label = row.get("label")
                split = row.get("split")
                is_correction = row.get("is_correction", False)
                if not text:
                    raise ValueError(f"Empty text at {path}:{line_number}")
                if label not in labels:
                    raise ValueError(
                        f"Unknown label {label!r} at {path}:{line_number}"
                    )
                if split not in SPLITS:
                    raise ValueError(
                        f"split must be one of {sorted(SPLITS)} at "
                        f"{path}:{line_number}"
                    )
                if not isinstance(is_correction, bool):
                    raise ValueError(
                        f"is_correction must be a boolean at {path}:{line_number}"
                    )
                normalized = NormalizeText(text)
                previous_split = normalized_splits.setdefault(normalized, split)
                if previous_split != split:
                    raise ValueError(
                        f"Duplicate text crosses dataset splits: {text!r}"
                    )
                rows.append(
                    {
                        "text": text,
                        "label": label,
                        "split": split,
                        "is_correction": is_correction,
                    }
                )

    if not rows:
        raise ValueError("No examples found")
    for split in SPLITS:
        split_labels = {row["label"] for row in rows if row["split"] == split}
        missing = set(labels) - split_labels
        if missing:
            raise ValueError(f"{split} is missing labels: {sorted(missing)}")
    return rows


def Softmax(logits):
    maximum = max(logits)
    exp_values = [math.exp(value - maximum) for value in logits]
    total = sum(exp_values)
    return [value / total for value in exp_values]


def TrainSoftmax(examples, class_names, seed, sample_weights=None):
    rng = random.Random(seed)
    weights = [[0.0] * (FEATURE_DIMENSIONS + 1) for _ in class_names]
    class_ids = {name: index for index, name in enumerate(class_names)}
    training = [
        (FeatureVector(row["text"]), class_ids[row["target"]], row["weight"])
        for row in examples
    ]
    total_steps = max(1, EPOCHS * len(training))
    step = 0
    for _ in range(EPOCHS):
        rng.shuffle(training)
        for features, target, row_weight in training:
            logits = [
                sum(weights[class_id][feature] * value for feature, value in features)
                + weights[class_id][FEATURE_DIMENSIONS]
                for class_id in range(len(class_names))
            ]
            probabilities = Softmax(logits)
            learning_rate = LEARNING_RATE * (1.0 - 0.5 * step / total_steps)
            for class_id, probability in enumerate(probabilities):
                gradient = (probability - (1.0 if class_id == target else 0.0))
                gradient *= row_weight
                class_weights = weights[class_id]
                for feature, value in features:
                    old_weight = class_weights[feature]
                    class_weights[feature] = old_weight - learning_rate * (
                        gradient * value + L2 * old_weight
                    )
                class_weights[FEATURE_DIMENSIONS] -= learning_rate * gradient
            step += 1
    return weights


def Predict(weights, text):
    features = FeatureVector(text)
    logits = [
        sum(weights[class_id][feature] * value for feature, value in features)
        + weights[class_id][FEATURE_DIMENSIONS]
        for class_id in range(len(weights))
    ]
    return Softmax(logits)


def Metrics(expected, predicted, labels):
    confusion = {label: {candidate: 0 for candidate in labels} for label in labels}
    for truth, guess in zip(expected, predicted):
        confusion[truth][guess] += 1
    accuracy = sum(truth == guess for truth, guess in zip(expected, predicted)) / max(
        1, len(expected)
    )
    f1_values = []
    for label in labels:
        true_positive = confusion[label][label]
        false_positive = sum(confusion[other][label] for other in labels if other != label)
        false_negative = sum(confusion[label][other] for other in labels if other != label)
        precision = true_positive / max(1, true_positive + false_positive)
        recall = true_positive / max(1, true_positive + false_negative)
        f1_values.append(2 * precision * recall / max(1e-12, precision + recall))
    return {
        "accuracy": accuracy,
        "macro_f1": sum(f1_values) / len(f1_values),
        "confusion_matrix": confusion,
    }


def EvaluateIntent(rows, weights, labels, split):
    selected = [row for row in rows if row["split"] == split]
    expected = [row["label"] for row in selected]
    predicted = [labels[max(range(len(labels)), key=lambda index: Predict(weights, row["text"])[index])]
                 for row in selected]
    return Metrics(expected, predicted, labels)


def EvaluateCorrection(rows, weights, split):
    selected = [row for row in rows if row["split"] == split]
    expected = [row["is_correction"] for row in selected]
    predicted = [Predict(weights, row["text"])[1] >= 0.65 for row in selected]
    true_positive = sum(truth and guess for truth, guess in zip(expected, predicted))
    false_positive = sum(not truth and guess for truth, guess in zip(expected, predicted))
    false_negative = sum(truth and not guess for truth, guess in zip(expected, predicted))
    precision = true_positive / max(1, true_positive + false_positive)
    recall = true_positive / max(1, true_positive + false_negative)
    return {
        "threshold": 0.65,
        "precision": precision,
        "recall": recall,
        "f1": 2 * precision * recall / max(1e-12, precision + recall),
        "true_positive": true_positive,
        "false_positive": false_positive,
        "false_negative": false_negative,
    }


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
        "--output",
        type=Path,
        default=script_dir.parent / "assets" / "intent_model.json",
    )
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    labels = LoadLabels(args.labels)
    rows = LoadRows([args.data, args.correction_data], labels)
    intent_training = [
        {"text": row["text"], "target": row["label"], "weight": 1.0}
        for row in rows
        if row["split"] == "train"
    ]
    correction_training = [
        {
            "text": row["text"],
            "target": "correction" if row["is_correction"] else "normal",
            "weight": 1.0,
        }
        for row in rows
        if row["split"] == "train"
    ]
    correction_counts = {
        name: sum(row["target"] == name for row in correction_training)
        for name in ("normal", "correction")
    }
    for row in correction_training:
        row["weight"] = len(correction_training) / (
            2 * correction_counts[row["target"]]
        )

    intent_weights = TrainSoftmax(intent_training, labels, args.seed)
    correction_weights = TrainSoftmax(
        correction_training, ["normal", "correction"], args.seed + 1
    )
    payload = {
        "version": 1,
        "algorithm": "hashed_character_ngram_softmax",
        "feature_dimensions": FEATURE_DIMENSIONS,
        "max_ngram": MAX_NGRAM,
        "normalization": "remove_whitespace_l2",
        "labels": labels,
        "intent_weights": intent_weights,
        "correction_labels": ["normal", "correction"],
        "correction_weights": correction_weights,
        "correction_threshold": 0.65,
        "confidence_threshold": 0.25,
        "training": {
            "seed": args.seed,
            "epochs": EPOCHS,
            "source_examples": {
                "intent": len([row for row in rows if row["split"] == "train"]),
                "correction_positive": sum(
                    row["split"] == "train" and row["is_correction"] for row in rows
                ),
            },
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )

    for split in ("validation", "test"):
        intent_metrics = EvaluateIntent(rows, intent_weights, labels, split)
        correction_metrics = EvaluateCorrection(rows, correction_weights, split)
        print(
            json.dumps(
                {
                    "split": split,
                    "intent_accuracy": intent_metrics["accuracy"],
                    "intent_macro_f1": intent_metrics["macro_f1"],
                    "correction": correction_metrics,
                },
                ensure_ascii=False,
            )
        )
    print(f"Wrote model: {args.output}")


if __name__ == "__main__":
    Main()
