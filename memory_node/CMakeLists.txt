cmake_minimum_required(VERSION 3.16)
project(memory_node LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(xiaocai_msgs REQUIRED)
find_package(Threads REQUIRED)
set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty")
include_directories(include "${VENDOR_DIR}" "${VENDOR_DIR}/spdlog-1.17.0/include" "${VENDOR_DIR}/tomlplusplus-3.4.0/include" "${CMAKE_CURRENT_SOURCE_DIR}/../common/include")
add_executable(memory_node
  src/main.cpp
  src/memory_config.cpp
  src/memory_manager.cpp
  src/memory_node.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/node_config_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp")
ament_target_dependencies(memory_node ament_index_cpp rclcpp xiaocai_msgs)
target_link_libraries(memory_node Threads::Threads)
target_link_libraries(memory_node sqlite3)
install(TARGETS memory_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
