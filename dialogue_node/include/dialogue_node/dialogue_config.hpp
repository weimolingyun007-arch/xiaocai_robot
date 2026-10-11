#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

struct DialogueSettings {
  std::string api_url{"https://api.deepseek.com/chat/completions"};
  std::string model{"deepseek-flash"};
};

class DialogueConfig : public xc::NodeConfigBase {
public:
  DialogueConfig();
  const DialogueSettings &settings() const noexcept { return settings_; }

private:
  DialogueSettings settings_;
};
