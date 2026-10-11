#pragma once

#include "common/log_manager.hpp"

#include <toml++/toml.hpp>

#include <filesystem>

namespace xc {

// Loads shared sections from a node's biz_config.toml. Node-specific config
// classes can inherit this class and read their own keys from document().
class NodeConfigBase {
public:
  explicit NodeConfigBase(std::filesystem::path config_path);
  virtual ~NodeConfigBase() = default;

  const LogManagerConfig &Logging() const noexcept { return logging_; }
  const std::filesystem::path &ConfigPath() const noexcept { return config_path_; }

protected:
  const toml::table &Document() const noexcept { return document_; }

private:
  void Load();

  std::filesystem::path config_path_;
  toml::table document_;
  LogManagerConfig logging_;
};

}  // namespace xc
