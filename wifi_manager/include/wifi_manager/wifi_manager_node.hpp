#pragma once

#include "common/log_manager.hpp"
#include "wifi_manager/wifi_config.hpp"
#include "xiaocai_msgs/msg/wifi_status.hpp"
#include "xiaocai_msgs/srv/connect_wifi.hpp"
#include "xiaocai_msgs/srv/get_wifi_list.hpp"
#include "xiaocai_msgs/srv/get_wifi_status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <thread>

namespace xiaocai_robot {

class WifiManagerNode : public rclcpp::Node {
public:
  WifiManagerNode();
  ~WifiManagerNode() override;

private:
  void PublishStatus();
  void ReconnectLoop();
  xiaocai_msgs::msg::WifiStatus ReadStatus();
  void HandleGetWifiList(
      const std::shared_ptr<xiaocai_msgs::srv::GetWifiList::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::GetWifiList::Response> response);
  void HandleConnectWifi(
      const std::shared_ptr<xiaocai_msgs::srv::ConnectWifi::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::ConnectWifi::Response> response);
  void HandleGetWifiStatus(
      const std::shared_ptr<xiaocai_msgs::srv::GetWifiStatus::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::GetWifiStatus::Response> response);

  WifiConfig wifi_config_;
  LogManager log_manager_;
  std::mutex status_sample_mutex_;
  bool previous_sample_valid_{false};
  bool previous_connected_{false};
  std::uint64_t previous_rx_bytes_{0};
  std::uint64_t previous_tx_bytes_{0};
  std::chrono::steady_clock::time_point previous_sample_time_{};
  std::mutex status_cache_mutex_;
  xiaocai_msgs::msg::WifiStatus last_status_;
  bool have_status_{false};
  std::mutex network_command_mutex_;
  std::mutex reconnect_wait_mutex_;
  std::condition_variable reconnect_wait_condition_;
  std::atomic<bool> stop_reconnect_worker_{false};
  std::atomic<bool> reconnecting_{false};
  std::thread reconnect_worker_;
  rclcpp::CallbackGroup::SharedPtr service_callback_group_;

  rclcpp::Publisher<xiaocai_msgs::msg::WifiStatus>::SharedPtr status_publisher_;
  rclcpp::Service<xiaocai_msgs::srv::GetWifiList>::SharedPtr
      get_wifi_list_service_;
  rclcpp::Service<xiaocai_msgs::srv::ConnectWifi>::SharedPtr
      connect_wifi_service_;
  rclcpp::Service<xiaocai_msgs::srv::GetWifiStatus>::SharedPtr
      get_wifi_status_service_;
  rclcpp::TimerBase::SharedPtr status_timer_;
};

} // namespace xiaocai_robot
