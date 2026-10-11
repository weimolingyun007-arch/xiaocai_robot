#pragma once

#include "common/node_config_base.hpp"

#include <filesystem>

class IntentConfig : public xc::NodeConfigBase {
public:
  IntentConfig();
  explicit IntentConfig(std::filesystem::path config_path);
};
