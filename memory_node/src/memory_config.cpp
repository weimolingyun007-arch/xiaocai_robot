#include "memory_node/memory_config.hpp"
#include <filesystem>

namespace xiaocai_robot {

MemoryConfig::MemoryConfig() : NodeConfigBase("memory_node") {
  const auto *section = Document()["memory"].as_table();
  if (!section)
    return;
  if (auto value = (*section)["db_path"].value<std::string>())
    settings_.db_path = *value;
}

} // namespace xiaocai_robot
