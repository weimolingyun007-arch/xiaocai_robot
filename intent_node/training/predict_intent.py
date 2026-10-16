#!/usr/bin/env python3
"""Run one text through a locally fine-tuned intent classifier."""

import argparse
import json
from pathlib import Path

import torch
from transformers import AutoModelForSequenceClassification, AutoTokenizer


def Main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-dir", type=Path, default=Path(__file__).parent / "output"
    )
    parser.add_argument("--text", required=True)
    parser.add_argument("--top-k", type=int, default=3)
    parser.add_argument("--max-length", type=int, default=96)
    args = parser.parse_args()

    tokenizer = AutoTokenizer.from_pretrained(args.model_dir)
    model = AutoModelForSequenceClassification.from_pretrained(args.model_dir)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model.to(device)
    model.eval()

    inputs = tokenizer(
        args.text,
        truncation=True,
        max_length=args.max_length,
        return_tensors="pt",
    ).to(device)
    with torch.inference_mode():
        probabilities = model(**inputs).logits.softmax(dim=-1)[0]

    top_k = min(max(1, args.top_k), probabilities.shape[0])
    scores, indices = probabilities.topk(top_k)
    results = [
        {
            "label": model.config.id2label[int(index)],
            "score": float(score),
        }
        for score, index in zip(scores.cpu(), indices.cpu())
    ]
    print(
        json.dumps(
            {"text": args.text, "predictions": results},
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    Main()
