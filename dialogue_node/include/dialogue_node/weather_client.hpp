#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace xiaocai_robot {

class WeatherClient {
public:
  using Cancelled = std::function<bool()>;
  using HttpGet =
      std::function<nlohmann::json(const std::string &, const Cancelled &)>;

  WeatherClient(std::string default_city, int timeout_seconds,
                HttpGet http_get = {});

  nlohmann::json Query(const std::string &city, int day_offset,
                       const Cancelled &cancelled = {}) const;

private:
  // Bounded HTTPS transport; an injected transport supports isolated tests.
  nlohmann::json RequestJson(const std::string &url,
                             const Cancelled &cancelled) const;

  std::string default_city_;
  int timeout_seconds_;
  HttpGet http_get_;
};

} // namespace xiaocai_robot
