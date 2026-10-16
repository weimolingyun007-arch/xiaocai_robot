cmake_minimum_required(VERSION 3.16)
project(wifi_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(xiaocai_msgs REQUIRED)
find_package(Threads REQUIRED)

set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty")
include_directories(
  include
  "${VENDOR_DIR}"
  "${VENDOR_DIR}/spdlog-1.17.0/include"
  "${VENDOR_DIR}/tomlplusplus-3.4.0/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/include"
)

add_executable(wifi_manager
  src/main.cpp
  src/wifi_config.cpp
  src/wifi_manager_node.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/node_config_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp"
)
ament_target_dependencies(wifi_manager ament_index_cpp rclcpp xiaocai_msgs)
target_link_libraries(wifi_manager Threads::Threads)

install(TARGETS wifi_manager DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
