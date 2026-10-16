cmake_minimum_required(VERSION 3.16)
project(display_node LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(X11 REQUIRED COMPONENTS Xft)
find_package(Freetype REQUIRED)
find_package(xiaocai_msgs REQUIRED)

set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty")
include_directories(
  include
  "${VENDOR_DIR}"
  "${VENDOR_DIR}/spdlog-1.17.0/include"
  "${VENDOR_DIR}/tomlplusplus-3.4.0/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/include"
)

add_executable(display_node
  src/main.cpp
  src/display_config.cpp
  src/display_node.cpp
  src/display_window.cpp
  src/lyrics_presenter.cpp
  "${VENDOR_DIR}/qrcodegen-1.8.0/qrcodegen.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/node_config_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp"
)
ament_target_dependencies(display_node ament_index_cpp rclcpp std_msgs xiaocai_msgs)
target_include_directories(display_node PRIVATE ${X11_INCLUDE_DIR} ${FREETYPE_INCLUDE_DIRS})
target_link_libraries(display_node ${X11_LIBRARIES} ${X11_Xft_LIB})

install(TARGETS display_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
