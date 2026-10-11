#include "dialogue_node/dialogue_node.hpp"

#include <rclcpp/rclcpp.hpp>

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<DialogueNode>());
  rclcpp::shutdown();
}
