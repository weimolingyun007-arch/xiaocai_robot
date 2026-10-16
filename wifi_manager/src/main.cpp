#include "wifi_manager/wifi_manager_node.hpp"

#include <csignal>
#include <rclcpp/executors/multi_threaded_executor.hpp>

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  rclcpp::init(argc, argv);
  rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(),
                                                    3);
  auto node = std::make_shared<xiaocai_robot::WifiManagerNode>();
  executor.add_node(node);
  executor.spin();
  rclcpp::shutdown();
  return 0;
}
