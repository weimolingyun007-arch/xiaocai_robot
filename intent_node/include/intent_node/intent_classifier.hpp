#pragma once

#include "defination/intent_type.hpp"
#include "defination/memory_operation.hpp"
#include "defination/music_action.hpp"
#include "defination/volume_action.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace xiaocai_robot {

struct ChatIntent {};

struct DialogueIntent {
  common::IntentType type;
};

struct MusicIntent {
  common::MusicAction action;
  std::string query;
};

struct VolumeIntent {
  common::VolumeAction action;
  int percent = 0;
};

struct MemoryIntent {
  common::MemoryOperation operation;
  std::string content;
  int delay_seconds = 0;
};

using IntentPayload = std::variant<ChatIntent, DialogueIntent, MusicIntent,
                                   VolumeIntent, MemoryIntent>;

struct IntentResult {
  std::string normalized_text;
  IntentPayload payload{ChatIntent{}};
};

struct IntentPrediction {
  IntentResult result;
  std::string label;
  double confidence = 0.0;
  double correction_confidence = 0.0;
  bool is_correction = false;
};

class IntentClassifier {
public:
  IntentClassifier(std::string model_path, double confidence_threshold,
                   double correction_threshold);

  IntentPrediction Classify(std::string text) const;

private:
  using WeightMatrix = std::vector<std::vector<double>>;

  std::vector<std::pair<std::size_t, double>>
  FeatureVector(const std::string &text) const;
  std::vector<double>
  Scores(const WeightMatrix &weights,
         const std::vector<std::pair<std::size_t, double>> &features) const;

  std::size_t feature_dimensions_;
  std::size_t max_ngram_;
  std::vector<std::string> labels_;
  WeightMatrix intent_weights_;
  WeightMatrix correction_weights_;
  double confidence_threshold_;
  double correction_threshold_;
};

} // namespace xiaocai_robot
