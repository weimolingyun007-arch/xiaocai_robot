# Tool Calls 验证记录

日期：2026-10-11。目标：RK3566 / AArch64 / ROS 2 Humble。

## 板端协议回归

`mock_deepseek.py` 模拟云端响应，`tool_calls.cpp` 实例化生产 Dialogue 类，并运行真实单线程 ROS 执行器、服务和消息反馈。外设/记忆/音乐的执行端在此测试中使用模拟节点，因此不会修改真实记忆、提醒和音量。

通过以下检查：

- 一个回复请求依次执行时间、音量查询/设置、记忆保存/删除/读取、提醒创建及音乐状态查询，并将八个结果回传模型。
- 进入待机后，已接受的请求继续处理。
- 工具调音量只生成一次最终播报。
- 未知工具、畸形 JSON、越界音量不会执行操作。
- 同一 `tool_call_id` 的重复修改只执行一次。
- 音乐工具匹配 `request_id`，忽略旧歌曲状态；成功播放后不发布会暂停音乐的确认语音。
- “闭嘴”取消正在进行的 HTTP，请求结束后不执行工具、不回复；再次唤醒后能继续工作。

## 真实 DeepSeek 和生产节点

通过 `/xiaocai_robot/speech/text` 注入测试文字，经过生产 `intent_node`、`dialogue_node` 和执行节点；这部分不验证麦克风识别率。

1. “告诉我现在几点，顺便读取一下当前音量，一句话回答。”：实际收到模型的 `get_current_time`、`get_volume` 调用；本地工具成功；回传后第二次 HTTP 200，回答“现在是10月11日星期日上午10点31分，当前音量为100%”。时间与板端、主机系统时间相符。
2. “请播放两只老虎，并且查询现在几点。”：实际模型调用 `control_music`、`get_current_time`，音乐节点反馈播放成功；回传后第二次 HTTP 200。mpv 确认 `idle-active=false`、`pause=false`，有非零播放进度，输出设备为 `alsa/plug:dmix`。
3. 部署后的三个修改节点二进制 SHA256 与主机构建产物一致；七个 ROS 节点在线，`xiaocai.service` 为 enabled/active。

首次真实请求遇到临时 DNS 解析失败（curl 6）；随后板端域名解析及 HTTPS 恢复，以上真实接口测试通过。网络请求没有使用代理或主机中转。

运行包：`xiaocai_robot_20261011_095158.tar.gz`，只含运行文件；板端目录为 `/home/lckfb/Documents/xiaocai_robot`，根目录包含 `bin`、`bringup`、`msgs`、`resource`、`thirdparty`。
