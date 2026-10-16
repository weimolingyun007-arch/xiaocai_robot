#include "wifi_manager/wifi_manager_node.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace xiaocai_robot {
namespace {

struct CommandResult {
  bool success{false};
  bool timed_out{false};
  std::string output;
};

void CloseFd(int &fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool ReadByteCounter(const std::string &interface_name, const char *direction,
                     std::uint64_t &bytes) {
  std::ifstream counter("/sys/class/net/" + interface_name + "/statistics/" +
                        direction + "_bytes");
  return static_cast<bool>(counter >> bytes);
}

CommandResult RunCommand(const std::vector<std::string> &arguments,
                         const std::string &standard_input,
                         std::chrono::milliseconds timeout) {
  CommandResult result;
  if (arguments.empty())
    return result;

  int input_pipe[2]{-1, -1};
  int output_pipe[2]{-1, -1};
  if (pipe2(input_pipe, O_CLOEXEC) != 0 || pipe2(output_pipe, O_CLOEXEC) != 0) {
    CloseFd(input_pipe[0]);
    CloseFd(input_pipe[1]);
    CloseFd(output_pipe[0]);
    CloseFd(output_pipe[1]);
    return result;
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    CloseFd(input_pipe[0]);
    CloseFd(input_pipe[1]);
    CloseFd(output_pipe[0]);
    CloseFd(output_pipe[1]);
    return result;
  }
  posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto &argument : arguments)
    argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);

  pid_t child = -1;
  const int spawn_error = posix_spawnp(&child, argv.front(), &actions, nullptr,
                                       argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  CloseFd(input_pipe[0]);
  CloseFd(output_pipe[1]);
  if (spawn_error != 0) {
    CloseFd(input_pipe[1]);
    CloseFd(output_pipe[0]);
    return result;
  }

  bool input_open = !standard_input.empty();
  bool output_open = true;
  bool child_exited = false;
  std::size_t input_offset = 0;
  int child_status = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (!SetNonBlocking(input_pipe[1]))
    input_open = false;
  if (!SetNonBlocking(output_pipe[0]))
    output_open = false;
  if (!input_open)
    CloseFd(input_pipe[1]);
  if (!output_open)
    CloseFd(output_pipe[0]);

  while (input_open || output_open || !child_exited) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline && !child_exited) {
      result.timed_out = true;
      kill(child, SIGKILL);
    }

    std::array<pollfd, 2> descriptors{};
    nfds_t descriptor_count = 0;
    int input_index = -1;
    int output_index = -1;
    if (input_open) {
      input_index = static_cast<int>(descriptor_count);
      descriptors[descriptor_count++] = {input_pipe[1], POLLOUT, 0};
    }
    if (output_open) {
      output_index = static_cast<int>(descriptor_count);
      descriptors[descriptor_count++] = {output_pipe[0], POLLIN, 0};
    }
    if (descriptor_count > 0) {
      const int poll_result = poll(descriptors.data(), descriptor_count, 50);
      if (poll_result > 0 && input_index >= 0 &&
          (descriptors[input_index].revents & (POLLOUT | POLLERR | POLLHUP))) {
        const auto remaining = standard_input.size() - input_offset;
        if (remaining == 0) {
          input_open = false;
          CloseFd(input_pipe[1]);
        } else {
          const auto written = write(
              input_pipe[1], standard_input.data() + input_offset, remaining);
          if (written > 0)
            input_offset += static_cast<std::size_t>(written);
          else if (written < 0 && errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
            input_open = false;
            CloseFd(input_pipe[1]);
          }
        }
      }
      if (poll_result > 0 && output_index >= 0 &&
          (descriptors[output_index].revents & (POLLIN | POLLERR | POLLHUP))) {
        std::array<char, 4096> buffer{};
        for (;;) {
          const auto count = read(output_pipe[0], buffer.data(), buffer.size());
          if (count > 0) {
            constexpr std::size_t kOutputLimit = 1024 * 1024;
            const auto size = static_cast<std::size_t>(count);
            if (result.output.size() < kOutputLimit)
              result.output.append(
                  buffer.data(),
                  std::min(size, kOutputLimit - result.output.size()));
          } else if (count == 0) {
            output_open = false;
            CloseFd(output_pipe[0]);
            break;
          } else {
            if (errno == EINTR)
              continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
              output_open = false;
              CloseFd(output_pipe[0]);
            }
            break;
          }
        }
      }
    }

    if (!child_exited) {
      const auto waited = waitpid(child, &child_status, WNOHANG);
      if (waited == child)
        child_exited = true;
      else if (waited < 0 && errno != EINTR)
        child_exited = true;
    }
    if (child_exited && input_open) {
      input_open = false;
      CloseFd(input_pipe[1]);
    }
  }

  CloseFd(input_pipe[1]);
  CloseFd(output_pipe[0]);
  if (!child_exited) {
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  result.success = !result.timed_out && WIFEXITED(child_status) &&
                   WEXITSTATUS(child_status) == 0;
  return result;
}

std::vector<std::string> ParseTerseFields(const std::string &line) {
  std::vector<std::string> fields(1);
  bool escaped = false;
  for (const char character : line) {
    if (escaped) {
      fields.back().push_back(character);
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == ':') {
      fields.emplace_back();
    } else {
      fields.back().push_back(character);
    }
  }
  if (escaped)
    fields.back().push_back('\\');
  return fields;
}

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

} // namespace

WifiManagerNode::WifiManagerNode()
    : Node("wifi_manager"), wifi_config_(),
      log_manager_("wifi_manager", wifi_config_.Logging()),
      service_callback_group_(
          create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)) {
  status_publisher_ = create_publisher<xiaocai_msgs::msg::WifiStatus>(
      "/xiaocai_robot/wifi/status", rclcpp::QoS(1).transient_local());
  get_wifi_list_service_ = create_service<xiaocai_msgs::srv::GetWifiList>(
      "/xiaocai_robot/wifi/get_list",
      [this](
          std::shared_ptr<xiaocai_msgs::srv::GetWifiList::Request> request,
          std::shared_ptr<xiaocai_msgs::srv::GetWifiList::Response> response) {
        HandleGetWifiList(request, response);
      },
      rmw_qos_profile_services_default, service_callback_group_);
  connect_wifi_service_ = create_service<xiaocai_msgs::srv::ConnectWifi>(
      "/xiaocai_robot/wifi/connect",
      [this](
          std::shared_ptr<xiaocai_msgs::srv::ConnectWifi::Request> request,
          std::shared_ptr<xiaocai_msgs::srv::ConnectWifi::Response> response) {
        HandleConnectWifi(request, response);
      },
      rmw_qos_profile_services_default, service_callback_group_);
  get_wifi_status_service_ = create_service<xiaocai_msgs::srv::GetWifiStatus>(
      "/xiaocai_robot/wifi/get_status",
      [this](std::shared_ptr<xiaocai_msgs::srv::GetWifiStatus::Request> request,
             std::shared_ptr<xiaocai_msgs::srv::GetWifiStatus::Response>
                 response) { HandleGetWifiStatus(request, response); },
      rmw_qos_profile_services_default, service_callback_group_);
  status_timer_ =
      create_wall_timer(std::chrono::seconds(1), [this] { PublishStatus(); });
  PublishStatus();

  if (wifi_config_.Settings().auto_reconnect)
    reconnect_worker_ = std::thread([this] { ReconnectLoop(); });
  LOG_INFO("Wi-Fi manager ready on interface {} (automatic reconnect: {})",
           wifi_config_.Settings().interface,
           wifi_config_.Settings().auto_reconnect ? "enabled" : "disabled");
}

WifiManagerNode::~WifiManagerNode() {
  stop_reconnect_worker_.store(true);
  reconnect_wait_condition_.notify_all();
  if (reconnect_worker_.joinable())
    reconnect_worker_.join();
}

xiaocai_msgs::msg::WifiStatus WifiManagerNode::ReadStatus() {
  std::lock_guard<std::mutex> lock(status_sample_mutex_);
  xiaocai_msgs::msg::WifiStatus status;
  const auto &interface_name = wifi_config_.Settings().interface;
  status.available = if_nametoindex(interface_name.c_str()) != 0;
  if (status.available) {
    int carrier = 0;
    std::ifstream("/sys/class/net/" + interface_name + "/carrier") >> carrier;
    status.link_up = carrier == 1;

    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) == 0) {
      for (auto *entry = addresses; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || interface_name != entry->ifa_name)
          continue;
        if (entry->ifa_addr->sa_family == AF_INET) {
          const auto *address =
              reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
          const auto host_address = ntohl(address->sin_addr.s_addr);
          if (status.link_up && host_address != 0 &&
              (host_address >> 24) != 127 && (host_address >> 16) != 0xa9fe) {
            status.connected = true;
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
              status.ipv4_address = text;
          }
        } else if (entry->ifa_addr->sa_family == AF_INET6) {
          const auto &address =
              reinterpret_cast<const sockaddr_in6 *>(entry->ifa_addr)
                  ->sin6_addr;
          if (status.link_up && !IN6_IS_ADDR_UNSPECIFIED(&address) &&
              !IN6_IS_ADDR_LOOPBACK(&address) &&
              !IN6_IS_ADDR_LINKLOCAL(&address))
            status.connected = true;
        }
      }
      freeifaddrs(addresses);
    }

    std::ifstream wireless("/proc/net/wireless");
    std::string line;
    while (std::getline(wireless, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      std::istringstream name(line.substr(0, colon));
      std::string interface;
      name >> interface;
      if (interface != interface_name)
        continue;
      std::istringstream fields(line.substr(colon + 1));
      std::string flags;
      double quality = 0, signal = 0;
      if (fields >> flags >> quality >> signal && std::isfinite(signal)) {
        if (signal > 0 && signal <= 255)
          signal -= 256;
        if (signal >= -120 && signal <= -1) {
          status.signal_known = true;
          status.strength = static_cast<std::uint8_t>(signal >= -55   ? 4
                                                      : signal >= -65 ? 3
                                                      : signal >= -75 ? 2
                                                                      : 1);
        }
      }
      break;
    }

    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
    const bool counters_available =
        ReadByteCounter(interface_name, "rx", rx_bytes) &&
        ReadByteCounter(interface_name, "tx", tx_bytes);
    const auto sample_time = std::chrono::steady_clock::now();
    if (counters_available && status.connected && previous_sample_valid_ &&
        previous_connected_ && rx_bytes >= previous_rx_bytes_ &&
        tx_bytes >= previous_tx_bytes_) {
      const double elapsed =
          std::chrono::duration<double>(sample_time - previous_sample_time_)
              .count();
      if (elapsed > 0.0 && std::isfinite(elapsed)) {
        status.speed_known = true;
        status.download_bytes_per_second =
            static_cast<double>(rx_bytes - previous_rx_bytes_) / elapsed;
        status.upload_bytes_per_second =
            static_cast<double>(tx_bytes - previous_tx_bytes_) / elapsed;
      }
    }

    previous_sample_valid_ = counters_available;
    previous_connected_ = status.connected;
    if (counters_available) {
      previous_rx_bytes_ = rx_bytes;
      previous_tx_bytes_ = tx_bytes;
      previous_sample_time_ = sample_time;
    }
  } else {
    previous_sample_valid_ = false;
    previous_connected_ = false;
  }
  status.timestamp = now();
  status.auto_reconnect_enabled = wifi_config_.Settings().auto_reconnect;
  status.reconnecting = reconnecting_.load();
  if (!status.available)
    status.reconnect_state = "unavailable";
  else if (status.connected)
    status.reconnect_state = "connected";
  else if (status.reconnecting)
    status.reconnect_state = "reconnecting";
  else
    status.reconnect_state = "disconnected";
  return status;
}

void WifiManagerNode::PublishStatus() {
  auto status = ReadStatus();
  {
    std::lock_guard<std::mutex> lock(status_cache_mutex_);
    last_status_ = status;
    have_status_ = true;
  }
  status_publisher_->publish(status);
}

void WifiManagerNode::HandleGetWifiStatus(
    const std::shared_ptr<xiaocai_msgs::srv::GetWifiStatus::Request>,
    std::shared_ptr<xiaocai_msgs::srv::GetWifiStatus::Response> response) {
  std::lock_guard<std::mutex> lock(status_cache_mutex_);
  response->success = have_status_;
  response->status = last_status_;
  if (!response->success)
    response->error = "Wi-Fi status is not available yet";
}

void WifiManagerNode::HandleGetWifiList(
    const std::shared_ptr<xiaocai_msgs::srv::GetWifiList::Request> request,
    std::shared_ptr<xiaocai_msgs::srv::GetWifiList::Response> response) {
  std::lock_guard<std::mutex> lock(network_command_mutex_);
  const auto &interface = wifi_config_.Settings().interface;
  if (request->rescan) {
    const auto scan = RunCommand({"nmcli", "--wait", "15", "device", "wifi",
                                  "rescan", "ifname", interface},
                                 {}, std::chrono::seconds(20));
    if (!scan.success) {
      response->success = false;
      response->error = "NetworkManager could not scan for Wi-Fi networks";
      return;
    }
  }

  const auto result =
      RunCommand({"nmcli", "--terse", "--escape", "yes", "--fields",
                  "SSID,SIGNAL,"
                  "SECURITY",
                  "device", "wifi", "list", "ifname", interface},
                 {}, std::chrono::seconds(15));
  if (!result.success) {
    response->success = false;
    response->error = "NetworkManager could not read the Wi-Fi list";
    return;
  }

  std::string line;
  std::istringstream output(result.output);
  while (std::getline(output, line)) {
    const auto fields = ParseTerseFields(line);
    if (fields.size() != 3 || fields[0].empty())
      continue;
    int signal = 0;
    try {
      signal = std::clamp(std::stoi(fields[1]), 0, 100);
    } catch (...) {
      continue;
    }
    const auto existing =
        std::find(response->ssids.begin(), response->ssids.end(), fields[0]);
    const bool secured = !fields[2].empty() && fields[2] != "--";
    if (existing == response->ssids.end()) {
      response->ssids.push_back(fields[0]);
      response->signal_percent.push_back(static_cast<std::uint8_t>(signal));
      response->secured.push_back(secured);
    } else {
      const auto index = static_cast<std::size_t>(
          std::distance(response->ssids.begin(), existing));
      response->signal_percent[index] = static_cast<std::uint8_t>(
          std::max(signal, static_cast<int>(response->signal_percent[index])));
      response->secured[index] = secured;
    }
  }
  response->success = true;
}

void WifiManagerNode::HandleConnectWifi(
    const std::shared_ptr<xiaocai_msgs::srv::ConnectWifi::Request> request,
    std::shared_ptr<xiaocai_msgs::srv::ConnectWifi::Response> response) {
  if (request->ssid.empty() || request->ssid.size() > 32 ||
      request->ssid.find('\0') != std::string::npos ||
      request->password.find('\0') != std::string::npos) {
    response->success = false;
    response->error = "Invalid Wi-Fi network name or password";
    return;
  }

  std::lock_guard<std::mutex> lock(network_command_mutex_);
  const auto &interface = wifi_config_.Settings().interface;
  const auto connect =
      RunCommand({"nmcli", "--wait", "30", "--ask", "device", "wifi", "connect",
                  request->ssid, "ifname", interface},
                 request->password + "\n", std::chrono::seconds(35));
  if (!connect.success) {
    response->success = false;
    response->error =
        connect.timed_out
            ? "Timed out while connecting to the Wi-Fi network"
            : "NetworkManager failed to connect to the Wi-Fi network";
    LOG_WARN("Failed to connect to the requested Wi-Fi network");
    return;
  }

  response->success = true;
  if (!wifi_config_.Settings().auto_reconnect)
    return;

  const auto uuid =
      RunCommand({"nmcli", "--terse", "--get-values", "GENERAL.CON-UUID",
                  "device", "show", interface},
                 {}, std::chrono::seconds(10));
  const auto connection_id = Trim(uuid.output);
  if (uuid.success && !connection_id.empty()) {
    const auto autoconnect =
        RunCommand({"nmcli", "connection", "modify", "uuid", connection_id,
                    "connection.autoconnect", "yes"},
                   {}, std::chrono::seconds(10));
    response->auto_reconnect_enabled = autoconnect.success;
  }
  if (!response->auto_reconnect_enabled) {
    response->error = "Connected, but the saved network profile could not "
                      "enable automatic reconnect";
    LOG_WARN("Connected to Wi-Fi, but could not enable profile autoconnect");
  } else {
    LOG_INFO("Connected to Wi-Fi network '{}'", request->ssid);
  }
}

void WifiManagerNode::ReconnectLoop() {
  const auto interval =
      std::chrono::seconds(wifi_config_.Settings().reconnect_interval_sec);
  std::unique_lock<std::mutex> wait_lock(reconnect_wait_mutex_);
  while (!stop_reconnect_worker_.load()) {
    if (reconnect_wait_condition_.wait_for(wait_lock, interval, [this] {
          return stop_reconnect_worker_.load();
        }))
      break;
    wait_lock.unlock();

    xiaocai_msgs::msg::WifiStatus current_status;
    {
      std::lock_guard<std::mutex> status_lock(status_cache_mutex_);
      current_status = last_status_;
    }
    if (current_status.available && !current_status.connected) {
      reconnecting_.store(true);
      {
        std::lock_guard<std::mutex> command_lock(network_command_mutex_);
        const auto result = RunCommand({"nmcli", "--wait", "15", "device", "up",
                                        wifi_config_.Settings().interface},
                                       {}, std::chrono::seconds(20));
        if (result.success)
          LOG_INFO("Automatic Wi-Fi reconnection succeeded");
      }
      reconnecting_.store(false);
    }

    wait_lock.lock();
  }
}

} // namespace xiaocai_robot
