#!/usr/bin/env python3
"""Run the trained intent and correction classifier on one utterance."""

import argparse
import json
from pathlib import Path

from train_intent_classifier import FeatureVector, Softmax


def Main():
    script_dir = Path(__file__).parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--text", required=True)
    parser.add_argument(
        "--model", type=Path, default=script_dir.parent / "assets" / "intent_model.json"
    )
    args = parser.parse_args()
    model = json.loads(args.model.read_text(encoding="utf-8"))
    features = FeatureVector(args.text)

    def GetProbabilities(weights):
        dimension = model["feature_dimensions"]
        logits = [
            sum(weights[class_index][feature] * value for feature, value in features)
            + weights[class_index][dimension]
            for class_index in range(len(weights))
        ]
        return Softmax(logits)

    intent_probabilities = GetProbabilities(model["intent_weights"])
    intent_index = max(
        range(len(intent_probabilities)), key=intent_probabilities.__getitem__
    )
    correction_probabilities = GetProbabilities(model["correction_weights"])
    print(
        json.dumps(
            {
                "text": args.text,
                "intent": model["labels"][intent_index],
                "confidence": intent_probabilities[intent_index],
                "correction_confidence": correction_probabilities[1],
                "is_correction": correction_probabilities[1]
                >= model["correction_threshold"],
            },
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    Main()
