# Tool Calls 验证记录

## 2026-10-28：天气工具、历史与输出格式

已使用生产 `WeatherClient`、`DialogueController` 完成以下检查：

- AArch64 `dialogue_node` 交叉编译通过，新增天气实现已加入 CMake，合并的编译数据库已更新。
- 独立天气组件测试覆盖缺少城市、默认城市、中文参数编码、重名城市、未找到城市、今天与第六天预报、空温度、越界湿度、网络失败和取消；错误数据不会变成虚构的零值。
- 隔离模型 HTTP 测试覆盖 `get_weather` 工具结果回传、城市追问、JSON 历史一致性、最终回答后的新会话、追问超时、新会话重试本机模型，以及本机格式失败后回退云端。
- 格式整理最多一次，单独请求禁用所有工具，整理失败的混合正文不会交给 TTS 或写入历史。
- 本机真实 C++ 天气客户端查询深圳成功；板端直接访问 Open-Meteo 地名服务成功。
- 真实 Gemma 联调：“深圳明天的天气怎么样？”实际调用 `get_weather`，工具成功后回答阴天、23.0–30.2°C、降水概率0%，整轮约9.2秒。这些是当时 API 返回的动态数据，不是测试桩或固定答案。
- 随后的新会话询问彩虹，模型正常解释白光折射和色散，约3.2秒，没有继续天气拒答。
- “今天天气怎么样？”在默认城市留空时先追问城市，回答“深圳”后实际调用天气工具并完成回答，城市补充轮约9秒。

上述独立联调使用本机生产业务组件和空记忆回调，直接连接正在运行的 Gemma；没有调用 DeepSeek，未向板端发布语音、音量、音乐或记忆命令。

### 板端增量部署与实际语音验证

2026-10-28 23:09 已将 `dialogue_node` 和对应 `biz_config.toml` 更新至 `/home/lckfb/xiaocai_robot`，重启 `xiaocai.service`。服务 active、开机自启 enabled，10 个节点保持运行，7 个核心节点就绪心跳齐全。此次只更新对话组件及天气配置。

- 部署二进制 SHA256：`32ca0903869e62f4714e9a898115bafb976992c67c03f7869c45977496bbda52`。
- 部署配置 SHA256：`4ba7f52cf7cbdff65a54fd9cfef8794a1c49062840b53c189990355cdd1c9f5a`；默认城市留空，天气请求超时8秒。
- 回滚备份：`/home/lckfb/.xiaocai_dialogue_backup_20261028_230925/`，包含更新前的二进制与配置。
- 临时 ROS 发布器直接向生产对话节点发送“深圳明天的天气怎么样？”，板端日志确认 `Gemma local completion: HTTP=200 curl=0`、`Local tool: name=get_weather ok=true`，随后实际播报“明天深圳的天气是阴天，最高气温30.2摄氏度，最低气温23.0摄氏度”。
- 语音播放成功并返回待机后，再次唤醒对话状态，发送“彩虹为什么会有7种颜色？”，模型回答白光分解和不同波长的解释，未重复拒答，实际语音再次播放成功并返回待机。
- 验证覆盖生产 ROS 对话、工具、TTS 和播放完成状态，不测试本次麦克风识别率。临时发布器和更新暂存目录已清理，回滚备份保留。

发现并处理的兼容限制：当前 `llama.cpp` Gemma 4 解析器在同时收到工具和 `response_format` 时，优先进入最终结构约束分支，导致工具调用不能正常输出。因此正常工具阶段不传最终 JSON Schema；只有无工具的格式整理阶段才施加结构约束。

组件测试运行入口：

```bash
python3 -B dialogue_node/tests/run_controller_tests.py \
  build/dialogue_checks/weather_test build/dialogue_checks/conversation_test
```

以上两个测试程序使用本机 C++17 编译生产组件；也可在目标平台以 `-DXIAOCAI_BUILD_TOOL_TESTS=ON` 构建 `dialogue_weather_test`、`dialogue_conversation_test`，再将对应程序路径传给同一入口。测试模型服务由入口在随机本地端口启动，密钥使用测试占位值，不访问外部模型服务。

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
