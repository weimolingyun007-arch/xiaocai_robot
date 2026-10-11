cmake_minimum_required(VERSION 3.16)
project(xiaocai_msgs)

find_package(ament_cmake REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/AudioPlay.msg"
  "msg/Intent.msg"
  "msg/MusicCommand.msg"
  "msg/MemoryCommand.msg"
  "msg/MemoryResult.msg"
  "msg/Reminder.msg"
  "msg/RobotState.msg"
  "msg/VolumeCommand.msg"
  "msg/MusicState.msg"
  "srv/GetVolume.srv"
  DEPENDENCIES builtin_interfaces
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
