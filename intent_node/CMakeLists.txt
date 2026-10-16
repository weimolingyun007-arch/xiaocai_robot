cmake_minimum_required(VERSION 3.16)
project(intent_node LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(xiaocai_msgs REQUIRED)
find_package(Threads REQUIRED)
set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty")
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
include_directories(
  include
  "${VENDOR_DIR}"
  "${VENDOR_DIR}/spdlog-1.17.0/include"
  "${VENDOR_DIR}/tomlplusplus-3.4.0/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/include")
add_executable(intent_node
  src/main.cpp
  src/intent_classifier.cpp
  src/intent_config.cpp
  src/intent_feedback_store.cpp
  src/intent_node.cpp
  "${COMMON_DIR}/src/node_config_base.cpp"
  "${COMMON_DIR}/src/log_manager.cpp")
ament_target_dependencies(intent_node ament_index_cpp rclcpp std_msgs xiaocai_msgs)
target_link_libraries(intent_node Threads::Threads)
target_link_libraries(intent_node sqlite3)
install(TARGETS intent_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
