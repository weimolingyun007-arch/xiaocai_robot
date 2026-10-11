#include "memory_node/memory_node.hpp"

#include <rclcpp/rclcpp.hpp>

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<MemoryNode>());
  rclcpp::shutdown();
}
