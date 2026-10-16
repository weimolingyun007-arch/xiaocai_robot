#pragma once

#include "ble_manager/ble_config.hpp"
#include "common/log_manager.hpp"
#include "defination/ble_status.hpp"
#include "xiaocai_msgs/msg/ble_received.hpp"
#include "xiaocai_msgs/msg/ble_status.hpp"
#include "xiaocai_msgs/srv/connect_ble.hpp"
#include "xiaocai_msgs/srv/disconnect_ble.hpp"
#include "xiaocai_msgs/srv/get_ble_status.hpp"
#include "xiaocai_msgs/srv/scan_ble.hpp"
#include "xiaocai_msgs/srv/send_ble_data.hpp"
#include "xiaocai_msgs/srv/set_ble_notification.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

namespace xiaocai_robot {

class BleManagerNode : public rclcpp::Node {
public:
  BleManagerNode();
  ~BleManagerNode() override;

private:
  class BluetoothCtlSession;

  xiaocai_msgs::msg::BleStatus ReadStatus();
  void PublishStatus();
  void
  HandleScan(const std::shared_ptr<xiaocai_msgs::srv::ScanBle::Request> request,
             std::shared_ptr<xiaocai_msgs::srv::ScanBle::Response> response);
  void HandleConnect(
      const std::shared_ptr<xiaocai_msgs::srv::ConnectBle::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::ConnectBle::Response> response);
  void HandleDisconnect(
      const std::shared_ptr<xiaocai_msgs::srv::DisconnectBle::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::DisconnectBle::Response> response);
  void HandleSend(
      const std::shared_ptr<xiaocai_msgs::srv::SendBleData::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::SendBleData::Response> response);
  void HandleNotification(
      const std::shared_ptr<xiaocai_msgs::srv::SetBleNotification::Request>
          request,
      std::shared_ptr<xiaocai_msgs::srv::SetBleNotification::Response>
          response);
  void HandleGetStatus(
      const std::shared_ptr<xiaocai_msgs::srv::GetBleStatus::Request> request,
      std::shared_ptr<xiaocai_msgs::srv::GetBleStatus::Response> response);
  void PublishReceived(const std::vector<std::uint8_t> &data);

  BleConfig ble_config_;
  LogManager log_manager_;
  std::unique_ptr<BluetoothCtlSession> bluetooth_;
  std::mutex operation_mutex_;
  std::mutex status_mutex_;
  xiaocai_msgs::msg::BleStatus last_status_;
  bool have_status_{false};
  std::string connected_address_hint_;
  std::string subscribed_address_;
  std::string subscribed_uuid_;
  rclcpp::CallbackGroup::SharedPtr service_callback_group_;
  rclcpp::Publisher<xiaocai_msgs::msg::BleStatus>::SharedPtr status_publisher_;
  rclcpp::Publisher<xiaocai_msgs::msg::BleReceived>::SharedPtr
      received_publisher_;
  rclcpp::Service<xiaocai_msgs::srv::ScanBle>::SharedPtr scan_service_;
  rclcpp::Service<xiaocai_msgs::srv::ConnectBle>::SharedPtr connect_service_;
  rclcpp::Service<xiaocai_msgs::srv::DisconnectBle>::SharedPtr
      disconnect_service_;
  rclcpp::Service<xiaocai_msgs::srv::SendBleData>::SharedPtr send_service_;
  rclcpp::Service<xiaocai_msgs::srv::SetBleNotification>::SharedPtr
      notification_service_;
  rclcpp::Service<xiaocai_msgs::srv::GetBleStatus>::SharedPtr
      get_status_service_;
  rclcpp::TimerBase::SharedPtr status_timer_;
};

} // namespace xiaocai_robot
