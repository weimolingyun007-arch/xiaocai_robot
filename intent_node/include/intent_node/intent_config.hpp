#pragma once

#include "common/node_config_base.hpp"

#include <filesystem>

namespace xiaocai_robot {

class IntentConfig : public NodeConfigBase {
public:
  IntentConfig();
  explicit IntentConfig(std::filesystem::path config_path);
};

} // namespace xiaocai_robot
