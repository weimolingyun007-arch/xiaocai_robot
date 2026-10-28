#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

namespace xiaocai_robot {

struct DialogueSettings {
  std::string local_api_url{"http://10.70.51.222:8081/v1/chat/completions"};
  std::string local_model{"gemma4-e4b"};
  std::int32_t local_connect_timeout_seconds{2};
  std::int32_t local_timeout_seconds{45};
  std::int32_t local_max_tokens{256};
  std::string weather_default_city{};
  std::int32_t weather_timeout_seconds{8};
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
