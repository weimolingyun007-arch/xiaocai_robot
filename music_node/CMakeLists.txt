cmake_minimum_required(VERSION 3.16)
project(music_node LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(xiaocai_msgs REQUIRED)
find_package(Threads REQUIRED)
include_directories(include "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty" "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/spdlog-1.17.0/include" "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/tomlplusplus-3.4.0/include" "${CMAKE_CURRENT_SOURCE_DIR}/../common/include")
add_executable(music_node
  src/main.cpp
  src/music_config.cpp
  src/music_controller.cpp
  src/music_lyrics.cpp
  src/music_lyrics_loader.cpp
  src/music_login_manager.cpp
  src/music_node.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/node_config_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp")
ament_target_dependencies(music_node ament_index_cpp rclcpp std_msgs xiaocai_msgs)
target_link_libraries(music_node Threads::Threads)
option(XIAOCAI_BUILD_MUSIC_TESTS "Build isolated music controller regression tests" OFF)
if(XIAOCAI_BUILD_MUSIC_TESTS)
  add_executable(music_controller_test
    tests/controller.cpp
    src/music_controller.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/src/log_manager.cpp")
  ament_target_dependencies(music_controller_test std_msgs)
  target_link_libraries(music_controller_test Threads::Threads)
  add_executable(music_integration_test tests/integration.cpp)
  ament_target_dependencies(music_integration_test rclcpp std_msgs xiaocai_msgs)
  target_link_libraries(music_integration_test Threads::Threads)
endif()
install(TARGETS music_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
ament_package()
