#include "intent_node/intent_node.hpp"

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<xiaocai_robot::IntentNode>());
  rclcpp::shutdown();
  return 0;
}
