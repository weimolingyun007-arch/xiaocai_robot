#include "intent_node/intent_node.hpp"

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<IntentNode>());
  rclcpp::shutdown();
  return 0;
}
