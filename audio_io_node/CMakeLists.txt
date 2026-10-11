cmake_minimum_required(VERSION 3.16)
project(audio_io_node LANGUAGES CXX)
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
include_directories(include "${VENDOR_DIR}" "${VENDOR_DIR}/spdlog-1.17.0/include" "${VENDOR_DIR}/tomlplusplus-3.4.0/include" "${CMAKE_CURRENT_SOURCE_DIR}/../common/include")
add_executable(audio_io_node
  src/main.cpp
  src/audio_io_config.cpp
  src/audio_io.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/node_config_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp")
ament_target_dependencies(audio_io_node ament_index_cpp rclcpp std_msgs xiaocai_msgs)
target_link_libraries(audio_io_node Threads::Threads)
install(TARGETS audio_io_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
