#!/usr/bin/env python3
"""Fine-tune hfl/rbt3 for the intent_node label set."""

import argparse
import json
import random
import unicodedata
from pathlib import Path

import numpy as np
from datasets import Dataset
from sklearn.metrics import accuracy_score, f1_score
from transformers import (
    AutoModelForSequenceClassification,
    AutoTokenizer,
    DataCollatorWithPadding,
    Trainer,
    TrainingArguments,
    set_seed,
)


SPLITS = {"train", "validation", "test"}


def LoadLabels(path):
    payload = json.loads(path.read_text(encoding="utf-8"))
    labels = [entry["name"] for entry in payload["labels"]]
    if not labels or len(labels) != len(set(labels)):
        raise ValueError(f"Invalid or duplicate labels in {path}")
    return labels


def LoadRows(path, labels):
    rows = []
    with path.open(encoding="utf-8") as dataset_file:
        for line_number, line in enumerate(dataset_file, start=1):
            if not line.strip():
                continue
            row = json.loads(line)
            text = row.get("text", "").strip()
            label = row.get("label")
            split = row.get("split")
            if not text:
                raise ValueError(f"Empty text at {path}:{line_number}")
            if label not in labels:
                raise ValueError(f"Unknown label {label!r} at {path}:{line_number}")
            if split not in SPLITS:
                raise ValueError(
                    f"split must be one of {sorted(SPLITS)} at {path}:{line_number}"
                )
            rows.append({"text": text, "label": label, "split": split})

    if not rows:
        raise ValueError(f"No examples found in {path}")

    normalized_splits = {}
    for row in rows:
        normalized = unicodedata.normalize("NFKC", row["text"])
        normalized = "".join(normalized.split())
        previous_split = normalized_splits.setdefault(normalized, row["split"])
        if previous_split != row["split"]:
            raise ValueError(
                f"Duplicate text crosses dataset splits: {row['text']!r}"
            )

    for split in SPLITS:
        split_labels = {row["label"] for row in rows if row["split"] == split}
        missing = set(labels) - split_labels
        if missing:
            raise ValueError(f"{split} is missing labels: {sorted(missing)}")
    return rows


def BuildDataset(rows, split, label2id, tokenizer, max_length):
    examples = [
        {"text": row["text"], "labels": label2id[row["label"]]}
        for row in rows
        if row["split"] == split
    ]
    dataset = Dataset.from_list(examples)
    return dataset.map(
        lambda batch: tokenizer(
            batch["text"], truncation=True, max_length=max_length
        ),
        batched=True,
    )


def Main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--data",
        type=Path,
        default=Path(__file__).parent / "data" / "intent_examples.jsonl",
    )
    parser.add_argument(
        "--labels",
        type=Path,
        default=Path(__file__).parent / "intent_labels.json",
    )
    parser.add_argument("--model-name", default="hfl/rbt3")
    parser.add_argument(
        "--output-dir", type=Path, default=Path(__file__).parent / "output"
    )
    parser.add_argument("--epochs", type=float, default=8)
    parser.add_argument("--learning-rate", type=float, default=2e-5)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--max-length", type=int, default=96)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    labels = LoadLabels(args.labels)
    rows = LoadRows(args.data, labels)
    label2id = {label: index for index, label in enumerate(labels)}
    id2label = {index: label for label, index in label2id.items()}
    set_seed(args.seed)
    random.seed(args.seed)
    np.random.seed(args.seed)

    tokenizer = AutoTokenizer.from_pretrained(args.model_name)
    model = AutoModelForSequenceClassification.from_pretrained(
        args.model_name,
        num_labels=len(labels),
        label2id=label2id,
        id2label=id2label,
    )

    train_dataset = BuildDataset(
        rows, "train", label2id, tokenizer, args.max_length
    )
    validation_dataset = BuildDataset(
        rows, "validation", label2id, tokenizer, args.max_length
    )
    data_collator = DataCollatorWithPadding(tokenizer=tokenizer)

    def ComputeMetrics(evaluation):
        logits, targets = evaluation
        predictions = np.argmax(logits, axis=1)
        return {
            "accuracy": accuracy_score(targets, predictions),
            "macro_f1": f1_score(
                targets,
                predictions,
                labels=list(range(len(labels))),
                average="macro",
                zero_division=0,
            ),
        }

    training_args = TrainingArguments(
        output_dir=str(args.output_dir / "checkpoints"),
        learning_rate=args.learning_rate,
        per_device_train_batch_size=args.batch_size,
        per_device_eval_batch_size=args.batch_size,
        num_train_epochs=args.epochs,
        weight_decay=0.01,
        warmup_ratio=0.1,
        eval_strategy="epoch",
        save_strategy="epoch",
        save_total_limit=2,
        load_best_model_at_end=True,
        metric_for_best_model="eval_macro_f1",
        greater_is_better=True,
        logging_strategy="epoch",
        report_to="none",
        seed=args.seed,
    )
    trainer = Trainer(
        model=model,
        args=training_args,
        train_dataset=train_dataset,
        eval_dataset=validation_dataset,
        tokenizer=tokenizer,
        data_collator=data_collator,
        compute_metrics=ComputeMetrics,
    )
    trainer.train()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    trainer.save_model(str(args.output_dir))
    tokenizer.save_pretrained(str(args.output_dir))
    (args.output_dir / "intent_labels.json").write_text(
        args.labels.read_text(encoding="utf-8"), encoding="utf-8"
    )
    print(f"Saved best validation checkpoint to {args.output_dir}")
    print("Run evaluate_intent_classifier.py on the held-out test split before use.")


if __name__ == "__main__":
    Main()
