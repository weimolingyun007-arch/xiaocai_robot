#pragma once

#include "common/node_config_base.hpp"

#include <cstdint>
#include <string>

struct MemorySettings {
  std::string db_path{"/home/lckfb/xiaocai_robot/resource/memory.sqlite3"};
};

class MemoryConfig : public xc::NodeConfigBase {
public:
  MemoryConfig();
  const MemorySettings &settings() const noexcept { return settings_; }

private:
  MemorySettings settings_;
};
