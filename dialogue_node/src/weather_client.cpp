#include "dialogue_node/weather_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <utility>

namespace xiaocai_robot {

namespace {
using json = nlohmann::json;
constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
constexpr auto kGeocodingUrl = "https://geocoding-api.open-meteo.com/v1/search";
constexpr auto kForecastUrl = "https://api.open-meteo.com/v1/forecast";

json Failure(const std::string &code, const std::string &error) {
  return {{"ok", false}, {"error_code", code}, {"error", error}};
}

bool IsCancelled(const WeatherClient::Cancelled &cancelled) {
  return cancelled && cancelled();
}

std::string Trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string UrlEncode(const std::string &value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
      result += static_cast<char>(c);
    else {
      result += '%';
      result += kHex[c >> 4];
      result += kHex[c & 15];
    }
  }
  return result;
}

std::string Description(int code) {
  switch (code) {
  case 0:
    return "晴";
  case 1:
    return "大部晴朗";
  case 2:
    return "局部多云";
  case 3:
    return "阴";
  case 45:
  case 48:
    return "雾";
  case 51:
  case 53:
  case 55:
    return "毛毛雨";
  case 56:
  case 57:
    return "冻毛毛雨";
  case 61:
    return "小雨";
  case 63:
    return "中雨";
  case 65:
    return "大雨";
  case 66:
  case 67:
    return "冻雨";
  case 71:
    return "小雪";
  case 73:
    return "中雪";
  case 75:
    return "大雪";
  case 77:
    return "雪粒";
  case 80:
  case 81:
  case 82:
    return "阵雨";
  case 85:
  case 86:
    return "阵雪";
  case 95:
    return "雷雨";
  case 96:
  case 99:
    return "雷雨伴冰雹";
  default:
    return "未知天气类型";
  }
}

json Location(const json &value) {
  return {{"city", value.at("name")},
          {"region", value.value("admin1", "")},
          {"country", value.value("country", "")}};
}

double Number(const json &value, double low, double high) {
  if (!value.is_number())
    throw std::runtime_error("weather provider returned a missing number");
  const auto number = value.get<double>();
  if (!std::isfinite(number) || number < low || number > high)
    throw std::runtime_error("weather provider returned an invalid number");
  return number;
}

struct Transfer {
  std::string response;
  WeatherClient::Cancelled cancelled;
};

size_t AppendResponse(char *ptr, size_t size, size_t count, void *data) {
  auto &response = static_cast<Transfer *>(data)->response;
  if (size && count > (kMaxResponseBytes - response.size()) / size)
    return 0;
  response.append(ptr, size * count);
  return size * count;
}

int AbortTransfer(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return IsCancelled(static_cast<Transfer *>(data)->cancelled) ? 1 : 0;
}
} // namespace

WeatherClient::WeatherClient(std::string default_city, int timeout_seconds,
                             HttpGet http_get)
    : default_city_(Trim(default_city)),
      timeout_seconds_(std::clamp(timeout_seconds, 1, 15)),
      http_get_(std::move(http_get)) {}

nlohmann::json WeatherClient::Query(const std::string &city, int day_offset,
                                    const Cancelled &cancelled) const {
  const auto name = Trim(city.empty() ? default_city_ : city);
  if (name.empty())
    return Failure("location_required", "请先询问用户要查询哪个城市。");
  if (name.size() > 128 || day_offset < 0 || day_offset > 6)
    return Failure("invalid_arguments",
                   "城市最长128字节，查询日期为今天起0到6天。");
  try {
    auto get = [&](const std::string &url) {
      if (IsCancelled(cancelled))
        throw std::runtime_error("cancelled");
      auto response =
          http_get_ ? http_get_(url, cancelled) : RequestJson(url, cancelled);
      if (IsCancelled(cancelled))
        throw std::runtime_error("cancelled");
      return response;
    };
    const auto places =
        get(std::string(kGeocodingUrl) + "?name=" + UrlEncode(name) +
            "&count=5&language=zh&format=json");
    if (!places.contains("results") || !places["results"].is_array() ||
        places["results"].empty())
      return Failure("location_not_found",
                     "未找到该城市，请补充省份或国家后重试。");
    json candidates = json::array();
    for (const auto &place : places["results"]) {
      const auto feature = place.value("feature_code", "");
      if (feature == "PPLC" || feature.rfind("PPLA", 0) == 0)
        candidates.push_back(place);
    }
    if (candidates.empty())
      candidates = places["results"];
    if (candidates.size() != 1) {
      auto result =
          Failure("location_ambiguous", "地名有多个匹配，请询问省份或国家。查询"
                                        "时可使用“城市,省份”或“城市,国家”。");
      result["candidates"] = json::array();
      for (const auto &place : candidates)
        result["candidates"].push_back(Location(place));
      return result;
    }
    const auto &place = candidates[0];
    const auto latitude = Number(place.at("latitude"), -90, 90);
    const auto longitude = Number(place.at("longitude"), -180, 180);
    const auto data = get(std::string(kForecastUrl) +
                          "?latitude=" + std::to_string(latitude) +
                          "&longitude=" + std::to_string(longitude) +
                          "&current=temperature_2m,apparent_temperature,"
                          "relative_humidity_2m,weather_code,wind_speed_10m"
                          "&daily=weather_code,temperature_2m_max,temperature_"
                          "2m_min,precipitation_probability_max"
                          "&forecast_days=7&timezone=auto&temperature_unit="
                          "celsius&wind_speed_unit=kmh");
    const auto &daily = data.at("daily");
    auto day = [&](const char *key) -> const json & {
      return daily.at(key).at(static_cast<std::size_t>(day_offset));
    };
    const auto code = static_cast<int>(Number(day("weather_code"), 0, 99));
    json result = {
        {"ok", true},
        {"source", "Open-Meteo"},
        {"source_url", "https://open-meteo.com/"},
        {"location", Location(place)},
        {"timezone", data.at("timezone").get<std::string>()},
        {"date", day("time").get<std::string>()},
        {"day_offset", day_offset},
        {"retrieved_at_unix", std::time(nullptr)},
        {"forecast",
         {{"weather_code", code},
          {"description", Description(code)},
          {"temperature_min_c", Number(day("temperature_2m_min"), -100, 70)},
          {"temperature_max_c", Number(day("temperature_2m_max"), -100, 70)},
          {"precipitation_probability_percent",
           day("precipitation_probability_max")}}}};
    if (!day("precipitation_probability_max").is_null())
      Number(day("precipitation_probability_max"), 0, 100);
    if (day_offset == 0) {
      const auto &current = data.at("current");
      const auto current_code =
          static_cast<int>(Number(current.at("weather_code"), 0, 99));
      result["current"] = {
          {"time", current.at("time").get<std::string>()},
          {"temperature_c", Number(current.at("temperature_2m"), -100, 70)},
          {"apparent_temperature_c",
           Number(current.at("apparent_temperature"), -120, 90)},
          {"humidity_percent",
           Number(current.at("relative_humidity_2m"), 0, 100)},
          {"wind_speed_kmh", Number(current.at("wind_speed_10m"), 0, 500)},
          {"weather_code", current_code},
          {"description", Description(current_code)}};
    }
    return result;
  } catch (const std::exception &error) {
    return Failure(IsCancelled(cancelled) ? "cancelled" : "weather_unavailable",
                   error.what());
  }
}

nlohmann::json WeatherClient::RequestJson(const std::string &url,
                                          const Cancelled &cancelled) const {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                           curl_easy_cleanup);
  if (!curl)
    throw std::runtime_error("weather HTTP initialization failed");
  Transfer transfer{{}, cancelled};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 3L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT,
                   static_cast<long>(timeout_seconds_));
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, AppendResponse);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, AbortTransfer);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
  const auto status = curl_easy_perform(curl.get());
  long http_status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);
  if (status != CURLE_OK || http_status != 200)
    throw std::runtime_error(
        "weather HTTP request failed: HTTP=" + std::to_string(http_status) +
        " curl=" + std::to_string(static_cast<int>(status)));
  auto response = json::parse(transfer.response, nullptr, false);
  if (!response.is_object() || response.value("error", false))
    throw std::runtime_error("weather provider returned an invalid response");
  return response;
}

} // namespace xiaocai_robot
