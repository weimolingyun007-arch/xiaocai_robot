#include "ble_manager/ble_manager_node.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace xiaocai_robot {
namespace {

struct CommandResult {
  bool success{false};
  std::string output;
};

struct DeviceInfo {
  std::string address;
  std::string name;
  std::int16_t rssi{0};
  bool rssi_known{false};
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

bool IsHex(char value) {
  return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool IsBluetoothAddress(const std::string &address) {
  if (address.size() != 17)
    return false;
  for (std::size_t index = 0; index < address.size(); ++index) {
    if (index % 3 == 2) {
      if (address[index] != ':')
        return false;
    } else if (!IsHex(address[index])) {
      return false;
    }
  }
  return true;
}

bool IsGattAttribute(const std::string &value) {
  if (value.empty() || value.size() > 36)
    return false;
  bool has_hex = false;
  for (char character : value) {
    if (IsHex(character)) {
      has_hex = true;
    } else if (character != '-' && character != 'x' && character != 'X') {
      return false;
    }
  }
  return has_hex;
}

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool HasYesValue(const std::string &output, const std::string &key) {
  std::istringstream lines(output);
  std::string line;
  const std::string prefix = key + ":";
  while (std::getline(lines, line)) {
    line = Trim(line);
    if (line.compare(0, prefix.size(), prefix) == 0)
      return Lower(Trim(line.substr(prefix.size()))) == "yes";
  }
  return false;
}

std::string ReadInfoValue(const std::string &output, const std::string &key) {
  std::istringstream lines(output);
  std::string line;
  const std::string prefix = key + ":";
  while (std::getline(lines, line)) {
    line = Trim(line);
    if (line.compare(0, prefix.size(), prefix) == 0)
      return Trim(line.substr(prefix.size()));
  }
  return {};
}

bool IsCommandFailure(const std::string &output) {
  const auto lowered = Lower(output);
  for (const auto *failure :
       {"failed", "not found", "not available", "not connected",
        "invalid command", "no default controller"}) {
    if (lowered.find(failure) != std::string::npos)
      return true;
  }
  return false;
}

bool FindAddress(const std::string &text, std::size_t from,
                 std::string &address, std::size_t &position) {
  for (std::size_t index = from; index + 17 <= text.size(); ++index) {
    const auto candidate = text.substr(index, 17);
    if (IsBluetoothAddress(candidate)) {
      address = candidate;
      position = index;
      return true;
    }
  }
  return false;
}

DeviceInfo ParseDeviceLine(const std::string &line) {
  DeviceInfo device;
  const auto marker = line.find("Device ");
  if (marker == std::string::npos)
    return device;
  std::size_t address_position = 0;
  if (!FindAddress(line, marker + 7, device.address, address_position))
    return {};

  auto name_start = address_position + device.address.size();
  while (name_start < line.size() &&
         std::isspace(static_cast<unsigned char>(line[name_start])))
    ++name_start;
  auto name_end = line.find(" RSSI:", name_start);
  if (name_end == std::string::npos)
    name_end = line.find(" TxPower:", name_start);
  if (name_end == std::string::npos)
    name_end = line.size();
  device.name = Trim(line.substr(name_start, name_end - name_start));

  const auto rssi_position = line.find("RSSI:");
  if (rssi_position != std::string::npos) {
    try {
      const int value = std::stoi(Trim(line.substr(rssi_position + 5)));
      if (value >= -127 && value <= 20) {
        device.rssi = static_cast<std::int16_t>(value);
        device.rssi_known = true;
      }
    } catch (...) {
    }
  }
  return device;
}

std::vector<DeviceInfo> ParseDevices(const std::string &output) {
  std::vector<DeviceInfo> devices;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    auto device = ParseDeviceLine(line);
    if (device.address.empty())
      continue;
    const auto found = std::find_if(
        devices.begin(), devices.end(), [&](const DeviceInfo &candidate) {
          return candidate.address == device.address;
        });
    if (found == devices.end())
      devices.push_back(std::move(device));
    else {
      if (!device.name.empty())
        found->name = std::move(device.name);
      if (device.rssi_known) {
        found->rssi = device.rssi;
        found->rssi_known = true;
      }
    }
  }
  return devices;
}

void MergeDevice(std::vector<DeviceInfo> &devices, DeviceInfo device) {
  if (device.address.empty())
    return;
  const auto found = std::find_if(devices.begin(), devices.end(),
                                  [&](const DeviceInfo &candidate) {
                                    return candidate.address == device.address;
                                  });
  if (found == devices.end()) {
    devices.push_back(std::move(device));
    return;
  }
  if (!device.name.empty())
    found->name = std::move(device.name);
  if (device.rssi_known) {
    found->rssi = device.rssi;
    found->rssi_known = true;
  }
}

} // namespace

class BleManagerNode::BluetoothCtlSession {
public:
  explicit BluetoothCtlSession(
      std::function<void(const std::vector<std::uint8_t> &)> receive_callback)
      : receive_callback_(std::move(receive_callback)) {
    Start();
  }

  ~BluetoothCtlSession() { Stop(); }

  bool Available() const noexcept { return available_.load(); }

  void SetScanning(bool scanning) {
    scanning_.store(scanning);
    if (scanning) {
      std::lock_guard<std::mutex> lock(device_mutex_);
      devices_.clear();
    }
  }

  bool Scanning() const noexcept { return scanning_.load(); }

  std::vector<DeviceInfo> Devices() const {
    std::lock_guard<std::mutex> lock(device_mutex_);
    return devices_;
  }

  CommandResult Execute(const std::string &command,
                        std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    CommandResult result;
    if (!Available() || input_fd_ < 0)
      return result;

    std::uint64_t prompt_before = 0;
    {
      std::lock_guard<std::mutex> output_lock(output_mutex_);
      output_.clear();
      prompt_before = prompt_count_;
    }

    const std::string line = command + "\n";
    std::size_t offset = 0;
    const auto write_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (offset < line.size()) {
      const auto count =
          write(input_fd_, line.data() + offset, line.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (std::chrono::steady_clock::now() >= write_deadline)
          return result;
        pollfd descriptor{input_fd_, POLLOUT, 0};
        poll(&descriptor, 1, 50);
        continue;
      }
      return result;
    }

    std::unique_lock<std::mutex> output_lock(output_mutex_);
    const bool reached_prompt =
        output_condition_.wait_for(output_lock, timeout, [&] {
          return prompt_count_ > prompt_before || reader_exited_;
        });
    result.success = reached_prompt && prompt_count_ > prompt_before;
    result.output = output_;
    return result;
  }

private:
  void Start() {
    int input_pipe[2]{-1, -1};
    int output_pipe[2]{-1, -1};
    if (pipe2(input_pipe, O_CLOEXEC) != 0 ||
        pipe2(output_pipe, O_CLOEXEC) != 0) {
      CloseFd(input_pipe[0]);
      CloseFd(input_pipe[1]);
      CloseFd(output_pipe[0]);
      CloseFd(output_pipe[1]);
      return;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
      CloseFd(input_pipe[0]);
      CloseFd(input_pipe[1]);
      CloseFd(output_pipe[0]);
      CloseFd(output_pipe[1]);
      return;
    }
    posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

    char *arguments[]{const_cast<char *>("bluetoothctl"), nullptr};
    const int spawn_error = posix_spawnp(&child_pid_, arguments[0], &actions,
                                         nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    CloseFd(input_pipe[0]);
    CloseFd(output_pipe[1]);
    if (spawn_error != 0) {
      CloseFd(input_pipe[1]);
      CloseFd(output_pipe[0]);
      child_pid_ = -1;
      return;
    }

    input_fd_ = input_pipe[1];
    output_fd_ = output_pipe[0];
    SetNonBlocking(input_fd_);
    if (!SetNonBlocking(output_fd_)) {
      Stop();
      return;
    }
    reader_thread_ = std::thread([this] { ReaderLoop(); });
    std::unique_lock<std::mutex> lock(output_mutex_);
    const bool prompt_ready =
        output_condition_.wait_for(lock, std::chrono::seconds(3), [&] {
          return prompt_count_ > 0 || reader_exited_;
        });
    available_.store(prompt_ready && prompt_count_ > 0);
  }

  void Stop() {
    available_.store(false);
    if (input_fd_ >= 0) {
      const char quit[] = "quit\n";
      (void)write(input_fd_, quit, sizeof(quit) - 1);
      CloseFd(input_fd_);
    }
    if (child_pid_ > 0) {
      int child_status = 0;
      bool exited = false;
      for (int attempt = 0; attempt < 30; ++attempt) {
        const auto waited = waitpid(child_pid_, &child_status, WNOHANG);
        if (waited == child_pid_ || (waited < 0 && errno != EINTR)) {
          exited = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!exited) {
        kill(child_pid_, SIGTERM);
        while (waitpid(child_pid_, &child_status, 0) < 0 && errno == EINTR) {
        }
      }
      child_pid_ = -1;
    }
    if (reader_thread_.joinable())
      reader_thread_.join();
    CloseFd(output_fd_);
  }

  void ReaderLoop() {
    bool in_escape = false;
    bool in_csi = false;
    std::string line;
    std::array<char, 2048> buffer{};
    for (;;) {
      pollfd descriptor{output_fd_, POLLIN, 0};
      const int poll_result = poll(&descriptor, 1, 200);
      if (poll_result < 0 && errno == EINTR)
        continue;
      if (poll_result < 0)
        break;
      if (poll_result == 0)
        continue;
      if ((descriptor.revents & (POLLIN | POLLERR | POLLHUP)) == 0)
        continue;

      const auto count = read(output_fd_, buffer.data(), buffer.size());
      if (count == 0)
        break;
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        break;
      }

      std::vector<std::uint8_t> received_data;
      bool have_received_data = false;
      {
        std::lock_guard<std::mutex> lock(output_mutex_);
        for (ssize_t index = 0; index < count; ++index) {
          const unsigned char character = static_cast<unsigned char>(
              buffer[static_cast<std::size_t>(index)]);
          if (in_escape) {
            if (character == '[') {
              in_csi = true;
              in_escape = false;
            } else {
              in_escape = false;
            }
            continue;
          }
          if (in_csi) {
            if (character >= 0x40 && character <= 0x7e)
              in_csi = false;
            continue;
          }
          if (character == 0x1b) {
            in_escape = true;
            continue;
          }
          if (character == '\r')
            continue;
          const char printable = static_cast<char>(character);
          if (output_.size() < 1024U * 1024U)
            output_.push_back(printable);
          line.push_back(printable);
          if (line.size() >= 2 && line.compare(line.size() - 2, 2, "# ") == 0) {
            ++prompt_count_;
            line.clear();
            output_condition_.notify_all();
          } else if (printable == '\n') {
            if (line.find("Value:") != std::string::npos) {
              const auto value_position = line.find("Value:") + 6;
              std::istringstream bytes(line.substr(value_position));
              std::string token;
              while (bytes >> token) {
                if (token.size() == 1 && IsHex(token[0]))
                  token = "0" + token;
                if (token.size() != 2 || !IsHex(token[0]) || !IsHex(token[1]))
                  continue;
                received_data.push_back(static_cast<std::uint8_t>(
                    std::strtoul(token.c_str(), nullptr, 16)));
              }
              have_received_data = !received_data.empty();
            }
            const auto device = ParseDeviceLine(line);
            if (Scanning() && !device.address.empty()) {
              std::lock_guard<std::mutex> device_lock(device_mutex_);
              MergeDevice(devices_, device);
            }
            line.clear();
          }
        }
      }
      if (have_received_data && receive_callback_)
        receive_callback_(received_data);
    }
    {
      std::lock_guard<std::mutex> lock(output_mutex_);
      reader_exited_ = true;
      available_.store(false);
      output_condition_.notify_all();
    }
  }

  std::function<void(const std::vector<std::uint8_t> &)> receive_callback_;
  std::atomic<bool> available_{false};
  std::atomic<bool> scanning_{false};
  int input_fd_{-1};
  int output_fd_{-1};
  pid_t child_pid_{-1};
  std::thread reader_thread_;
  mutable std::mutex device_mutex_;
  std::vector<DeviceInfo> devices_;
  std::mutex command_mutex_;
  std::mutex output_mutex_;
  std::condition_variable output_condition_;
  std::string output_;
  std::uint64_t prompt_count_{0};
  bool reader_exited_{false};
};

BleManagerNode::BleManagerNode()
    : Node("ble_manager"), ble_config_(),
      log_manager_("ble_manager", ble_config_.Logging()),
      service_callback_group_(
          create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)) {
  bluetooth_ = std::make_unique<BluetoothCtlSession>(
      [this](const std::vector<std::uint8_t> &data) { PublishReceived(data); });
  status_publisher_ = create_publisher<xiaocai_msgs::msg::BleStatus>(
      "/xiaocai_robot/ble/status", rclcpp::QoS(1).transient_local());
  received_publisher_ = create_publisher<xiaocai_msgs::msg::BleReceived>(
      "/xiaocai_robot/ble/received", 10);
  scan_service_ = create_service<xiaocai_msgs::srv::ScanBle>(
      "/xiaocai_robot/ble/scan",
      [this](const std::shared_ptr<xiaocai_msgs::srv::ScanBle::Request> request,
             std::shared_ptr<xiaocai_msgs::srv::ScanBle::Response> response) {
        HandleScan(request, response);
      },
      rmw_qos_profile_services_default, service_callback_group_);
  connect_service_ = create_service<xiaocai_msgs::srv::ConnectBle>(
      "/xiaocai_robot/ble/connect",
      [this](
          const std::shared_ptr<xiaocai_msgs::srv::ConnectBle::Request> request,
          std::shared_ptr<xiaocai_msgs::srv::ConnectBle::Response> response) {
        HandleConnect(request, response);
      },
      rmw_qos_profile_services_default, service_callback_group_);
  disconnect_service_ = create_service<xiaocai_msgs::srv::DisconnectBle>(
      "/xiaocai_robot/ble/disconnect",
      [this](const std::shared_ptr<xiaocai_msgs::srv::DisconnectBle::Request>
                 request,
             std::shared_ptr<xiaocai_msgs::srv::DisconnectBle::Response>
                 response) { HandleDisconnect(request, response); },
      rmw_qos_profile_services_default, service_callback_group_);
  send_service_ = create_service<xiaocai_msgs::srv::SendBleData>(
      "/xiaocai_robot/ble/send",
      [this](
          const std::shared_ptr<xiaocai_msgs::srv::SendBleData::Request>
              request,
          std::shared_ptr<xiaocai_msgs::srv::SendBleData::Response> response) {
        HandleSend(request, response);
      },
      rmw_qos_profile_services_default, service_callback_group_);
  notification_service_ = create_service<xiaocai_msgs::srv::SetBleNotification>(
      "/xiaocai_robot/ble/notify",
      [this](
          const std::shared_ptr<xiaocai_msgs::srv::SetBleNotification::Request>
              request,
          std::shared_ptr<xiaocai_msgs::srv::SetBleNotification::Response>
              response) { HandleNotification(request, response); },
      rmw_qos_profile_services_default, service_callback_group_);
  get_status_service_ = create_service<xiaocai_msgs::srv::GetBleStatus>(
      "/xiaocai_robot/ble/get_status",
      [this](
          const std::shared_ptr<xiaocai_msgs::srv::GetBleStatus::Request>
              request,
          std::shared_ptr<xiaocai_msgs::srv::GetBleStatus::Response> response) {
        HandleGetStatus(request, response);
      },
      rmw_qos_profile_services_default, service_callback_group_);
  status_timer_ =
      create_wall_timer(std::chrono::seconds(2), [this] { PublishStatus(); });
  PublishStatus();
  LOG_INFO("BLE manager ready (bluetoothctl {})",
           bluetooth_->Available() ? "connected" : "unavailable");
}

BleManagerNode::~BleManagerNode() { bluetooth_.reset(); }

xiaocai_msgs::msg::BleStatus BleManagerNode::ReadStatus() {
  xiaocai_msgs::msg::BleStatus status;
  status.state =
      static_cast<std::uint8_t>(common::BleStatusState::Disconnected);
  if (!bluetooth_ || !bluetooth_->Available())
    return status;

  const auto adapter = bluetooth_->Execute("show", std::chrono::seconds(3));
  if (!adapter.success)
    return status;
  const bool adapter_available =
      adapter.output.find("Controller ") != std::string::npos;
  const bool adapter_powered = HasYesValue(adapter.output, "Powered");
  const bool scanning =
      bluetooth_->Scanning() || HasYesValue(adapter.output, "Discovering");
  if (!adapter_available || !adapter_powered)
    return status;
  if (scanning)
    status.state = static_cast<std::uint8_t>(common::BleStatusState::Scanning);

  const auto check_connection = [&](const std::string &address,
                                    const std::string &fallback_name) {
    if (!IsBluetoothAddress(address))
      return false;
    const auto info =
        bluetooth_->Execute("info " + address, std::chrono::seconds(3));
    if (!info.success || !HasYesValue(info.output, "Connected"))
      return false;
    status.state = static_cast<std::uint8_t>(common::BleStatusState::Connected);
    status.connected_address = address;
    status.connected_name = ReadInfoValue(info.output, "Alias");
    if (status.connected_name.empty())
      status.connected_name = ReadInfoValue(info.output, "Name");
    if (status.connected_name.empty())
      status.connected_name = fallback_name;
    return true;
  };

  if (!check_connection(connected_address_hint_, {})) {
    const auto known_devices =
        bluetooth_->Execute("devices", std::chrono::seconds(3));
    for (const auto &device : ParseDevices(known_devices.output)) {
      if (device.address == connected_address_hint_)
        continue;
      if (check_connection(device.address, device.name))
        break;
    }
  }
  return status;
}

void BleManagerNode::PublishStatus() {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  auto status = ReadStatus();
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    last_status_ = status;
    have_status_ = true;
  }
  status_publisher_->publish(status);
}

void BleManagerNode::HandleScan(
    const std::shared_ptr<xiaocai_msgs::srv::ScanBle::Request> request,
    std::shared_ptr<xiaocai_msgs::srv::ScanBle::Response> response) {
  std::unique_lock<std::mutex> operation_lock(operation_mutex_);
  if (!bluetooth_->Available()) {
    response->error = "bluetoothctl is unavailable";
    return;
  }
  const auto duration = std::clamp<unsigned int>(request->duration_sec, 1, 30);
  bluetooth_->SetScanning(true);
  auto scanning_status = last_status_;
  if (scanning_status.state !=
      static_cast<std::uint8_t>(common::BleStatusState::Connected))
    scanning_status.state =
        static_cast<std::uint8_t>(common::BleStatusState::Scanning);
  status_publisher_->publish(scanning_status);
  bool scan_started = true;
  for (const auto &command : {"menu scan", "transport le", "back"}) {
    const auto result = bluetooth_->Execute(command, std::chrono::seconds(3));
    scan_started =
        scan_started && result.success && !IsCommandFailure(result.output);
  }
  const auto start = bluetooth_->Execute("scan on", std::chrono::seconds(3));
  scan_started =
      scan_started && start.success && !IsCommandFailure(start.output);
  if (scan_started)
    std::this_thread::sleep_for(std::chrono::seconds(duration));
  const auto stop = bluetooth_->Execute("scan off", std::chrono::seconds(4));
  bluetooth_->SetScanning(false);
  const auto list = bluetooth_->Execute("devices", std::chrono::seconds(3));
  auto devices = bluetooth_->Devices();
  for (auto &device : ParseDevices(list.output))
    MergeDevice(devices, std::move(device));

  response->success =
      scan_started && stop.success && !IsCommandFailure(stop.output);
  for (const auto &device : devices) {
    xiaocai_msgs::msg::BleScanResult message;
    message.address = device.address;
    message.name = device.name;
    message.rssi = device.rssi;
    message.rssi_known = device.rssi_known;
    response->scan_list.devices.push_back(std::move(message));
  }
  if (!response->success)
    response->error = "BLE scan could not be started or stopped";
  else if (response->scan_list.devices.empty())
    response->error = "No BLE devices found during the scan";
  operation_lock.unlock();
  PublishStatus();
}

void BleManagerNode::HandleConnect(
    const std::shared_ptr<xiaocai_msgs::srv::ConnectBle::Request> request,
    std::shared_ptr<xiaocai_msgs::srv::ConnectBle::Response> response) {
  std::unique_lock<std::mutex> operation_lock(operation_mutex_);
  if (!IsBluetoothAddress(request->address)) {
    response->error = "Invalid Bluetooth address";
    return;
  }
  const auto result = bluetooth_->Execute("connect " + request->address,
                                          std::chrono::seconds(30));
  if (!result.success || IsCommandFailure(result.output)) {
    response->error = Trim(result.output);
    if (response->error.empty())
      response->error = "Bluetooth connection command timed out";
    return;
  }
  const auto info =
      bluetooth_->Execute("info " + request->address, std::chrono::seconds(4));
  response->success = info.success && HasYesValue(info.output, "Connected");
  response->name = ReadInfoValue(info.output, "Alias");
  if (response->name.empty())
    response->name = ReadInfoValue(info.output, "Name");
  if (response->success)
    connected_address_hint_ = request->address;
  if (!response->success)
    response->error = "The device did not report a connected state";
  operation_lock.unlock();
  PublishStatus();
}

void BleManagerNode::HandleDisconnect(
    const std::shared_ptr<xiaocai_msgs::srv::DisconnectBle::Request> request,
    std::shared_ptr<xiaocai_msgs::srv::DisconnectBle::Response> response) {
  std::unique_lock<std::mutex> operation_lock(operation_mutex_);
  if (!IsBluetoothAddress(request->address)) {
    response->error = "Invalid Bluetooth address";
    return;
  }
  const auto result = bluetooth_->Execute("disconnect " + request->address,
                                          std::chrono::seconds(10));
  const auto info =
      bluetooth_->Execute("info " + request->address, std::chrono::seconds(4));
  response->success = result.success &&
                      (HasYesValue(info.output, "Connected") == false) &&
                      !IsCommandFailure(result.output);
  if (!response->success)
    response->error = Trim(result.output);
  if (response->success) {
    if (connected_address_hint_ == request->address)
      connected_address_hint_.clear();
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    subscribed_address_.clear();
    subscribed_uuid_.clear();
  }
  operation_lock.unlock();
  PublishStatus();
}

void BleManagerNode::HandleSend(
    const std::shared_ptr<xiaocai_msgs::srv::SendBleData::Request> request,
    std::shared_ptr<xiaocai_msgs::srv::SendBleData::Response> response) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  if (!IsBluetoothAddress(request->address) ||
      !IsGattAttribute(request->characteristic_uuid) || request->data.empty() ||
      request->data.size() > 512) {
    response->error = "Invalid address, characteristic, or payload size";
    return;
  }
  const auto info =
      bluetooth_->Execute("info " + request->address, std::chrono::seconds(4));
  if (!info.success || !HasYesValue(info.output, "Connected")) {
    response->error = "BLE device is not connected";
    return;
  }

  std::ostringstream data;
  data << std::hex;
  for (std::size_t index = 0; index < request->data.size(); ++index) {
    if (index > 0)
      data << ' ';
    data.width(2);
    data.fill('0');
    data << static_cast<unsigned int>(request->data[index]);
  }
  std::string output;
  bool success = true;
  const std::vector<std::string> commands{
      "menu gatt", "select-attribute " + request->characteristic_uuid,
      "write " + data.str(), "back"};
  for (const auto &command : commands) {
    const auto result = bluetooth_->Execute(command, std::chrono::seconds(8));
    output += result.output;
    if (!result.success || IsCommandFailure(result.output)) {
      success = false;
      break;
    }
  }
  response->success = success;
  if (!success)
    response->error = Trim(output);
}

void BleManagerNode::HandleNotification(
    const std::shared_ptr<xiaocai_msgs::srv::SetBleNotification::Request>
        request,
    std::shared_ptr<xiaocai_msgs::srv::SetBleNotification::Response> response) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  if (!IsBluetoothAddress(request->address) ||
      !IsGattAttribute(request->characteristic_uuid)) {
    response->error = "Invalid address or characteristic";
    return;
  }
  const auto info =
      bluetooth_->Execute("info " + request->address, std::chrono::seconds(4));
  if (!info.success || !HasYesValue(info.output, "Connected")) {
    response->error = "BLE device is not connected";
    return;
  }

  if (request->enabled) {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    if (!subscribed_address_.empty() &&
        (subscribed_address_ != request->address ||
         subscribed_uuid_ != request->characteristic_uuid)) {
      response->error = "Only one BLE notification can be active at a time";
      return;
    }
    subscribed_address_ = request->address;
    subscribed_uuid_ = request->characteristic_uuid;
  }

  const std::string command = request->enabled ? "notify on" : "notify off";
  bool success = true;
  std::string output;
  const std::vector<std::string> steps{
      "menu gatt", "select-attribute " + request->characteristic_uuid, command,
      "back"};
  for (const auto &step : steps) {
    const auto result = bluetooth_->Execute(step, std::chrono::seconds(8));
    output += result.output;
    if (!result.success || IsCommandFailure(result.output)) {
      success = false;
      break;
    }
  }
  response->success = success;
  if (!success) {
    response->error = Trim(output);
    if (request->enabled) {
      std::lock_guard<std::mutex> status_lock(status_mutex_);
      subscribed_address_.clear();
      subscribed_uuid_.clear();
    }
  } else if (!request->enabled) {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    if (subscribed_address_ == request->address &&
        subscribed_uuid_ == request->characteristic_uuid) {
      subscribed_address_.clear();
      subscribed_uuid_.clear();
    }
  }
}

void BleManagerNode::HandleGetStatus(
    const std::shared_ptr<xiaocai_msgs::srv::GetBleStatus::Request>,
    std::shared_ptr<xiaocai_msgs::srv::GetBleStatus::Response> response) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  response->status = ReadStatus();
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    last_status_ = response->status;
    have_status_ = true;
  }
}

void BleManagerNode::PublishReceived(const std::vector<std::uint8_t> &data) {
  xiaocai_msgs::msg::BleReceived message;
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    if (subscribed_address_.empty() || subscribed_uuid_.empty())
      return;
    message.address = subscribed_address_;
    message.characteristic_uuid = subscribed_uuid_;
  }
  message.timestamp = now();
  message.data = data;
  received_publisher_->publish(message);
}

} // namespace xiaocai_robot
