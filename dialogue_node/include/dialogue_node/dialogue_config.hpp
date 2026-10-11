#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

namespace xiaocai_robot {

struct DialogueSettings {
  std::string api_url{"https://api.deepseek.com/chat/completions"};
  std::string model{"deepseek-flash"};
};

class DialogueConfig : public NodeConfigBase {
public:
  DialogueConfig();
  const DialogueSettings &Settings() const noexcept { return settings_; }

private:
  DialogueSettings settings_;
};

} // namespace xiaocai_robot
