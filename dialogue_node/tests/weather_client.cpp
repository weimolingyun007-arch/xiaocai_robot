#include "dialogue_node/weather_client.hpp"

#include <curl/curl.h>

#include <iostream>
#include <stdexcept>

namespace {
using json = nlohmann::json;

void Require(bool condition, const char *label) {
  if (!condition)
    throw std::runtime_error(label);
}

json Place(const char *name = "深圳", const char *region = "广东") {
  return {{"name", name},        {"admin1", region},
          {"country", "中国"},   {"latitude", 22.54},
          {"longitude", 114.06}, {"feature_code", "PPLA2"}};
}

json Forecast() {
  json daily = {{"time", json::array()},
                {"weather_code", json::array()},
                {"temperature_2m_max", json::array()},
                {"temperature_2m_min", json::array()},
                {"precipitation_probability_max", json::array()}};
  for (int i = 0; i < 7; ++i) {
    daily["time"].push_back("2026-10-" + std::to_string(20 + i));
    daily["weather_code"].push_back(61);
    daily["temperature_2m_max"].push_back(28 + i);
    daily["temperature_2m_min"].push_back(20 + i);
    daily["precipitation_probability_max"].push_back(70);
  }
  return {{"timezone", "Asia/Shanghai"},
          {"daily", daily},
          {"current",
           {{"time", "2026-10-20T12:00"},
            {"temperature_2m", 27},
            {"apparent_temperature", 28},
            {"relative_humidity_2m", 60},
            {"weather_code", 2},
            {"wind_speed_10m", 9}}}};
}
} // namespace

int main(int argc, char **argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  try {
    if (argc > 1) {
      xiaocai_robot::WeatherClient real("", 10);
      const auto result = real.Query(argv[1], 0);
      std::cout << result.dump(2) << '\n';
      Require(result.value("ok", false), "live weather lookup failed");
    } else {
      int calls = 0;
      bool encoded_city = false;
      bool cancel = false;
      json places = {{"results", {Place()}}};
      auto forecast = Forecast();
      auto get = [&](const std::string &url, const auto &) {
        ++calls;
        if (url.find("geocoding-api") != std::string::npos) {
          encoded_city = url.find("%E6%B7%B1%E5%9C%B3") != std::string::npos;
          return places;
        }
        return forecast;
      };
      xiaocai_robot::WeatherClient client("", 8, get);
      Require(client.Query("", 0)["error_code"] == "location_required" &&
                  calls == 0,
              "missing location must not query or guess");
      Require(client.Query("深圳", 7)["error_code"] == "invalid_arguments" &&
                  calls == 0,
              "forecast range validated before HTTP");
      Require(client.Query(std::string(129, 'x'), 0)["error_code"] ==
                  "invalid_arguments",
              "oversized city rejected");
      auto result = client.Query("深圳", 0);
      Require(result["ok"] && encoded_city && calls == 2 &&
                  result["current"]["temperature_c"] == 27 &&
                  result["forecast"]["description"] == "小雨",
              "Chinese geocoding, current conditions and forecast normalize");
      result = client.Query("深圳", 6);
      Require(
          result["ok"] && !result.contains("current") &&
              result["date"] == "2026-10-26" &&
              result["forecast"]["temperature_max_c"] == 34,
          "forecast date and last supported day match selected array index");
      xiaocai_robot::WeatherClient configured("深圳", 8, get);
      Require(configured.Query("", 1)["ok"], "configured default city works");
      places["results"].push_back(Place("深圳", "福建"));
      Require(client.Query("深圳", 0)["error_code"] == "location_ambiguous",
              "ambiguous administrative cities request clarification");
      places = {{"results", json::array()}};
      Require(client.Query("未找到", 0)["error_code"] == "location_not_found",
              "no match must not invent coordinates");
      places = {{"results", {Place()}}};
      forecast["daily"]["temperature_2m_max"][0] = nullptr;
      Require(client.Query("深圳", 0)["error_code"] == "weather_unavailable",
              "null temperature does not become zero");
      forecast = Forecast();
      forecast["current"]["relative_humidity_2m"] = 200;
      Require(!client.Query("深圳", 0)["ok"].get<bool>(),
              "invalid provider value rejected");
      calls = 0;
      cancel = true;
      Require(client.Query("深圳", 0, [&] { return cancel; })["error_code"] ==
                      "cancelled" &&
                  calls == 0,
              "cancelled queries never access HTTP");
      xiaocai_robot::WeatherClient broken(
          "深圳", 8, [](const auto &, const auto &) -> json {
            throw std::runtime_error("timeout");
          });
      Require(broken.Query("", 0)["error_code"] == "weather_unavailable",
              "transport failure returned as a tool error");
      std::cout << "PASS weather input, forecast, ambiguity, provider errors "
                   "and cancellation\n";
    }
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    curl_global_cleanup();
    return 1;
  }
  curl_global_cleanup();
  return 0;
}
