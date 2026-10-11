#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

namespace xiaocai_robot {

struct MemorySettings {
  std::string db_path{"/home/lckfb/xiaocai_robot/resource/memory.sqlite3"};
};

class MemoryConfig : public NodeConfigBase {
public:
  MemoryConfig();
  const MemorySettings &Settings() const noexcept { return settings_; }

private:
  MemorySettings settings_;
};

} // namespace xiaocai_robot
