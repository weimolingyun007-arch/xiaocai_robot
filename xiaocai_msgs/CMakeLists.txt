cmake_minimum_required(VERSION 3.16)
project(xiaocai_msgs)

find_package(ament_cmake REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/AudioPlay.msg"
  "msg/AudioPlaybackResult.msg"
  "msg/ConversationState.msg"
  "msg/ConversationTurn.msg"
  "msg/TtsRequest.msg"
  "msg/TtsResult.msg"
  "msg/Intent.msg"
  "msg/MusicCommand.msg"
  "msg/MemoryCommand.msg"
  "msg/MemoryResult.msg"
  "msg/Reminder.msg"
  "msg/RobotState.msg"
  "msg/VolumeCommand.msg"
  "msg/MusicState.msg"
  "msg/MusicLyrics.msg"
  "msg/MusicLogin.msg"
  "msg/WifiStatus.msg"
  "msg/BleStatus.msg"
  "msg/BleScanResult.msg"
  "msg/BleScanList.msg"
  "msg/BleReceived.msg"
  "srv/GetVolume.srv"
  "srv/GetWifiList.srv"
  "srv/ConnectWifi.srv"
  "srv/GetWifiStatus.srv"
  "srv/ScanBle.srv"
  "srv/ConnectBle.srv"
  "srv/DisconnectBle.srv"
  "srv/SendBleData.srv"
  "srv/SetBleNotification.srv"
  "srv/GetBleStatus.srv"
  DEPENDENCIES builtin_interfaces
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
