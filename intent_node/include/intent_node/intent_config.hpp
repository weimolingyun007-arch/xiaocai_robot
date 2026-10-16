#pragma once

#include "common/node_config_base.hpp"

#include <filesystem>
#include <string>

namespace xiaocai_robot {

struct IntentSettings {
  bool feedback_collection_enabled{true};
  std::string feedback_database_path{
      "/userdata/config/intent_node/intent_feedback.sqlite3"};
  std::string model_path{"intent/intent_model.json"};
  double confidence_threshold{0.25};
  double correction_threshold{0.65};
};

class IntentConfig : public NodeConfigBase {
public:
  IntentConfig();
  explicit IntentConfig(std::filesystem::path config_path);

  const IntentSettings &Settings() const noexcept { return settings_; }

private:
  void LoadSettings();

  IntentSettings settings_;
};

} // namespace xiaocai_robot
