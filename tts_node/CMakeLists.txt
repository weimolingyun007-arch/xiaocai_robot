cmake_minimum_required(VERSION 3.16)
project(tts_node LANGUAGES CXX)
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
set(SHERPA_ONNX_DIR "${VENDOR_DIR}/sherpa-onnx")
set(ONNXRUNTIME_DIR "${VENDOR_DIR}/onnxruntime")
include_directories(include "${VENDOR_DIR}" "${VENDOR_DIR}/spdlog-1.17.0/include" "${VENDOR_DIR}/tomlplusplus-3.4.0/include" "${SHERPA_ONNX_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common/include")
add_executable(tts_node
  src/main.cpp
  src/tts_config.cpp
  src/tts_node.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/node_config_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp")
ament_target_dependencies(tts_node ament_index_cpp rclcpp std_msgs xiaocai_msgs)
target_link_libraries(tts_node Threads::Threads)
target_link_libraries(tts_node
  "${SHERPA_ONNX_DIR}/libsherpa-onnx-c-api.so"
  "${ONNXRUNTIME_DIR}/libonnxruntime.so")
install(TARGETS tts_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
