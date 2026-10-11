#include "intent_node/intent_config.hpp"

#include <utility>

namespace xiaocai_robot {

IntentConfig::IntentConfig() : NodeConfigBase("intent_node") {}

IntentConfig::IntentConfig(std::filesystem::path config_path)
    : NodeConfigBase(std::move(config_path)) {}

} // namespace xiaocai_robot
