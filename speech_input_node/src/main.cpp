#include "speech_input_node/speech_input_node.hpp"

#include <rclcpp/rclcpp.hpp>

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<SpeechInputNode>());
  rclcpp::shutdown();
}
