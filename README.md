# 小蔡机器人 C++ 节点工程

适用：RK3566 泰山派、Ubuntu 22.04、ROS 2 Humble、RK809 音频小板 A。

## 工程结构

七个 ROS 2 节点目录直接放在工程根目录。每个节点目录包含自己的 `include/`、`src/`、`CMakeLists.txt` 和 `package.xml`。

| 目录 | 职责 |
| --- | --- |
| `audio_io_node` | 咪头采样、喇叭播放与音量控制 |
| `speech_input_node` | SenseVoice 中文语音识别、Vosk 唤醒词检测 |
| `intent_node` | 判断音乐、音量、免打扰、记忆、提醒和闲聊意图 |
| `dialogue_node` | 本地功能协调、DeepSeek 对话及本地工具调用 |
| `memory_node` | SQLite 记忆、免打扰状态和提醒 |
| `tts_node` | sherpa-onnx 离线中文语音合成 |
| `music_node` | 调用网易云 ncm-cli 搜歌、播放及控制 |
| `common` | 节点共享的配置基类、日志管理、业务枚举和 MPV 辅助函数 |
| `thirdparty` | C++ 头文件和 ARM64 动态库 |
| `xiaocai_msgs` | ROS 2 自定义消息和服务接口源码 |
| `bringup` | 编译、启动、停止和 systemd 开机自启脚本 |
| `docs` | 节点代码结构和编写约定 |
| `build` | 每个节点由 CMake 和 Make 生成的可执行文件 |

源码目录内的模型文件放在使用它们的节点目录下。板端运行时，DeepSeek 密钥和记忆数据库分别放在 `resource/deepseek.env` 和 `resource/memory.sqlite3`，文件权限为 600。首次更新时部署脚本会从原来的 `dialogue_node/.secrets/`、`memory_node/data/` 迁移数据。

### 节点职责与配置

ROS 节点类负责 ROS 通信和生命周期，将复杂业务交给专用组件处理：`speech_input_node` 使用 `SpeechInputController` 管理唤醒、录音和识别流程；`intent_node` 使用 `IntentRules` 解析本地指令；`dialogue_node` 使用 `DialogueController` 管理对话和模型工具调用；`music_node` 使用 `MusicController` 执行音乐任务；`memory_node` 使用 `MemoryManager` 管理 SQLite 记忆和提醒。各节点的配置解析由对应的配置类完成，共享的 `NodeConfigBase` 负责 TOML 文件定位和日志配置读取。

每个节点的业务配置位于 `<node_package>/config/biz_config.toml`。运行包会将它们放在 `config/<node_package>/biz_config.toml`，启动脚本通过 `XIAOCAI_CONFIG_ROOT` 指向这个配置根目录；未设置该变量时，节点从 ROS 安装的包共享目录读取配置。日志使用 spdlog，由 `common` 中的 `LogManager` 管理。默认写入 `/userdata/log/<node_name>`，单文件上限 20 MB，每天最多 3 个日志文件，保留 7 天；具体值可在节点配置的 `[logging]` 表中调整。

## 编译和运行

工程使用同一个脚本识别构建环境。在工程目录运行 `make build`，会调用 `bringup/ota/build.sh`：

```bash
make build
```

在 RK3566 板端，脚本使用板端 ROS 2 Humble、CMake 和 Make 原生编译；在 x86_64 电脑上，脚本使用 `/opt/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu` 交叉编译。产物都位于 `build/<节点名>/<节点名>`。

电脑端交叉编译还需要 RK3566 的 AArch64 ROS/sysroot，默认位置是 `~/.cache/xiaocai_robot/aarch64-sysroot`；也可以通过 `XIAOCAI_SYSROOT` 指定其他位置。交叉编译产物不会自动部署到板端。运行时需能解析并访问 `api.deepseek.com`。

在板端编译完成后启动节点：

```bash
bash bringup/scripts/start_all_node.sh
```

语音输入节点在待机时只监听唤醒词。每次唤醒后只接收一条用户请求，收到后立即回到待唤醒状态，不播放叮声；这条请求的处理和语音回复继续正常进行，继续提问需要再次唤醒。“小蔡，播放晴天”这种唤醒词与请求连说的方式同样只接受一条请求。已排队或仍在识别中的旧音频不会成为下一条请求。唤醒后一直没有收到请求时，安静 120 秒也会返回待机；时长可通过 `speech_input_node` 的 `inactivity_timeout_sec` 启动参数设置。“闭嘴”继续用于立即停止回复和音乐。

## DeepSeek 调用板端工具

普通聊天和复杂请求由 `dialogue_node` 发送给 DeepSeek，并携带本地工具声明。模型返回 `tool_calls` 后，C++ 程序校验参数并执行对应功能，把真实结果以 `role: tool` 和 `tool_call_id` 回传，再请求模型生成最终回答。这是 [DeepSeek 官方 Tool Calls 协议](https://api-docs.deepseek.com/guides/tool_calls/)；模型不会自己登录板子或执行 shell。

| 工具名 | 本地执行方式 |
| --- | --- |
| `get_current_time` | C++ 读取板端系统时钟，返回北京时间、日期、星期；准确性依赖板端时钟 |
| `get_volume` | 调用 `/xiaocai_robot/audio/get_volume` 服务 |
| `set_volume` | 发布 `/xiaocai_robot/audio/volume/command`，等待实际音量反馈；范围 0–150，超过100时语音和音乐一起增强，最多 +6 dB |
| `control_music` | 发布 `MusicCommand`，等待 `music_node` 返回相同请求编号的状态；支持播放、暂停、继续、停止、上/下一首 |
| `get_music_status` | 读取 `music_node` 最新播放状态、歌曲名和进度 |
| `save_memory` / `forget_memory` / `list_memories` | 请求 `memory_node`，等待 SQLite 操作结果 |
| `create_reminder` | 请求 `memory_node` 保存延时提醒；最长 30 天 |

例如“一次请求：现在几点，顺便告诉我音量”，模型可返回：

```json
{"tool_calls":[
  {"id":"call_time","type":"function","function":{"name":"get_current_time","arguments":"{}"}},
  {"id":"call_volume","type":"function","function":{"name":"get_volume","arguments":"{}"}}
]}
```

板端查询后逐条回传结果，比如 `{"ok":true,"percent":120}`，模型再结合实测时间和音量回答。简单音量、音乐、记忆和提醒命令仍通过本地规则直接执行；带“顺便、并且、然后”等的组合请求交给模型，避免只执行其中一项。目前没有实时天气查询工具。

HTTP 和工具等待使用独立工作线程，ROS 主线程继续处理服务反馈。“闭嘴”和新的唤醒会取消旧的云端任务；一次请求后的自动休眠不会取消该任务。最多 4 轮工具执行、8 次工具调用；对参数和工具名称进行本地校验，不提供任意 shell 工具。同一 `tool_call_id` 不会重复执行。超时可能已经产生效果的修改操作不会由程序自动重试。

成功播放或继续音乐后，歌曲作为声音反馈，不再朗读模型最终确认，避免 TTS 把刚开始的音乐暂停。模型仍收到工具结果并生成最终确认；播放失败由对话节点根据错误回复。`MusicCommand` 和 `MusicState` 的 `request_id` 用于匹配工具请求，普通本地命令为 0。

回归测试源码位于 `dialogue_node/tests/`：模拟 DeepSeek HTTP 协议，但使用真实 ROS 执行器和消息/服务验证工具执行、错误参数、重复调用、音乐反馈关联及取消。可在已配置的 CMake 构建目录启用 `-DXIAOCAI_BUILD_TOOL_TESTS=ON`，构建 `dialogue_tool_calls_test`；测试要在隔离 ROS_DOMAIN_ID 下运行，启动 `mock_deepseek.py`，并把测试节点的 `api_url` 设为 `http://127.0.0.1:18763`，设置虚拟 `DEEPSEEK_API_KEY`。默认编译和运行包不包含测试程序。

## 打包和部署

在电脑端进入工程目录后运行：

```bash
make build
make ota
make deploy
```

`make build` 编译节点。`make ota` 只打包现有编译产物，不会触发编译；它在工程根目录生成 `xiaocai_robot_时间.tar.gz`。压缩包根目录为 `xiaocai_robot/`，其下有 `bin/`、`config/`、`resource/`、`bringup/`、`thirdparty/` 和 `msgs/`。七个可执行文件集中在 `bin/`；七个节点的 `biz_config.toml` 放在 `config/<节点名>/`，启动脚本通过 `XIAOCAI_CONFIG_ROOT` 加载；语音模型和唤醒提示音分别放在 `resource/speech/`、`resource/tts/`；第三方依赖库放在 `thirdparty/`；自定义消息与服务生成的 ROS 2 类型支持库放在 `msgs/`。`bringup/` 包含启停脚本和 systemd 服务单元。接口定义保留在工程源码 `xiaocai_msgs/msg/` 和 `xiaocai_msgs/srv/`，生成的 C++ 头文件在 `build/install/include/`；这些内容不随运行包部署。压缩包不含节点源码、头文件或构建配置。打包校验成功后会删除旧压缩包，只保留最新包。

`make deploy` 调用 ADB 部署脚本，将最新压缩包传到板端 `/home/lckfb/xiaocai_robot`，替换整个运行目录并重启服务；它不会重新编译或打包。板端运行目录包含 `bin/`、`config/`、`resource/`、`bringup/`、`thirdparty/` 和 `msgs/`。

部署脚本通过 ADB 校验压缩包后停止服务，将原工作目录移到隐藏备份，再替换运行目录并安装新的 systemd 服务单元。它从旧目录复制密钥与数据库到新版的 `resource/`，并保留其内容；这两个运行数据文件不包含在压缩包中。部署失败会恢复原工作目录和服务单元。首次迁移时，运行目录从 `/home/lckfb/Documents/xiaocai_robot` 移到 `/home/lckfb/xiaocai_robot`。ADB shell 需要有 root 权限。成功部署后，上一版目录保存在 `/home/lckfb/.xiaocai_robot_runtime_backup_时间`，脚本只保留最近一份备份。

如果部署前已经在板端 shell 中进入旧工作目录，部署后该 shell 仍停留在被移动的备份目录；执行 `cd /home/lckfb/xiaocai_robot` 后再运行 `ls`，才能查看新运行目录。

停止全部节点：

```bash
bash bringup/scripts/stop_all_node.sh
```

## 开机自启

安装并启动 systemd 服务：

```bash
sudo bash bringup/ota/install_autostart.sh
```

查看服务状态：`sudo systemctl status xiaocai.service`。手动停止或启动：`sudo systemctl stop xiaocai.service` / `sudo systemctl start xiaocai.service`。取消开机自启并停止：`sudo systemctl disable --now xiaocai.service`。

## 主要话题

- `/xiaocai_robot/audio/mic`：咪头音频数据
- `/xiaocai_robot/audio/playing`：喇叭播放状态
- `/xiaocai_robot/audio/volume/state`：音量变化事件
- `/xiaocai_robot/audio/volume/command`：音量设置和增减命令，类型为 `xiaocai_msgs/msg/VolumeCommand`
- `/xiaocai_robot/speech/text`：唤醒后的语音识别文字
- `/xiaocai_robot/speech/awake`：唤醒状态
- `/xiaocai_robot/intent`：对话意图，类型为 `xiaocai_msgs/msg/Intent`，只承载闲聊、音量查询、免打扰开关和提醒澄清
- `/xiaocai_robot/music/command`：音乐操作，类型为 `xiaocai_msgs/msg/MusicCommand`，字段 `action`、`query`、`request_id`
- `/xiaocai_robot/tts/text`：待合成的回复文字
- `/xiaocai_robot/audio/play`：待播放 WAV 路径，类型为 `xiaocai_msgs/msg/AudioPlay`；`path` 为空时停止播放并清空待播队列
- `/xiaocai_robot/state`：机器人状态，类型为 `xiaocai_msgs/msg/RobotState`，字段 `state`
- `/xiaocai_robot/memory/command`：记忆操作请求，类型为 `xiaocai_msgs/msg/MemoryCommand`，字段 `operation`、`request_id`、`content`、`delay_seconds`、`enabled`
- `/xiaocai_robot/memory/result`：记忆操作结果，类型为 `xiaocai_msgs/msg/MemoryResult`，字段 `operation`、`request_id`、`ok`、`error`、`facts`、`content`、`deleted`、`enabled`
- `/xiaocai_robot/reminder/due`：到期提醒，类型为 `xiaocai_msgs/msg/Reminder`，字段 `id`、`content`
- `/xiaocai_robot/music/state`：音乐状态，类型为 `xiaocai_msgs/msg/MusicState`，字段 `state`、`title`、`position_seconds`、`duration_seconds`、`error`、`request_id`
- `/xiaocai_robot/audio/get_volume`：读取当前音量的服务（`xiaocai_msgs/srv/GetVolume`）

`Intent.type` 使用 `uint8` 承载 `xiaocai_robot::common::IntentType` 的枚举值；该枚举只表示对话节点处理的闲聊、音量查询、免打扰开关和提醒澄清，名称和值定义在 `common/include/defination/intent_type.hpp`。音乐、音量调节、记忆和提醒创建分别通过 `MusicCommand`、`VolumeCommand`、`MemoryCommand` 传递，并使用 `MusicAction`、`VolumeAction`、`MemoryOperation` 表示操作类型；这些枚举定义在 `common/include/defination/`。`MusicCommand.request_id` 与 `MusicState.request_id` 使用 `uint64`；`0` 表示普通语音命令，音乐工具请求使用非零编号并由 `MusicState` 原样回传。所有 `xiaocai_msgs` 消息和服务请求的首字段均为 `builtin_interfaces/Time timestamp`，发送时填写本节点 ROS 时钟。

所有机器人话题和服务统一使用 `/xiaocai_robot/` 前缀。对话意图、音乐操作、音量操作和记忆操作使用各自的自定义命令消息，避免把无关业务参数混装在一个通用消息中。状态与操作枚举统一在 `common/include/defination/` 的 C++ 枚举中定义，由消息的整数型字段承载；`.msg` 不声明重复的枚举常量。每个自定义消息以及服务请求部分的首字段都是 `builtin_interfaces/Time timestamp`。自定义接口定义位于 `xiaocai_msgs/msg/` 与 `xiaocai_msgs/srv/`，由 `make build` 先生成，再编译依赖节点。

## 网易云语音点歌

板端需已安装 `/usr/local/bin/ncm-cli`（官方 `@music163/ncm-cli`）及 `mpv`，并在 `lckfb` 用户下配置开放平台凭证和扫码登录。CLI 运行时安装在 `/opt/ncm-cli`，账号配置位于 `/home/lckfb/.config/ncm-cli/`，更新机器人运行包时会保留。机器人依赖的 CLI、mpv 与 ROS 2 一样由板端预先安装。

唤醒后可以说“播放晴天”“放一首歌”“暂停音乐”“继续播放”“停止播放音乐”“下一首”“上一首”。不指定歌名时搜索轻音乐。`intent_node` 发布 `MusicCommand`，用 `MusicAction` 表示动作、`query` 表示歌名或关键词；`music_node` 处理命令，点歌时会把搜索结果里后续最多 5 首可播放歌曲加入网易云队列，并循环播放队列。唤醒词会结束当前连续播放。点歌失败时本地语音提示，不请求 DeepSeek。

音量范围是 0–150，当前配置 `audio_io_node/config/biz_config.toml` 的启动默认值是 80，可通过 `volume_percent` 调整。RK809 DAC 在 100% 已达硬件满量程；100% 以上通过共享 ALSA `softvol` PCM 对语音和音乐一起增加数字增益，最高额外 +6 dB。唤醒应答文件 `wake_ack.wav` 的音频峰值约为 -6.2 dBFS，为最大增益留有余量。唤醒后可通过“音量调低一点”逐步调整，也可指定 150%。

音乐播放期间，`speech_input_node` 不运行 SenseVoice 语音转写，只用 Vosk 检测唤醒词和“闭嘴”，避免把歌曲识别成对话。叫“小蔡小蔡”或“小蔡同学”会结束当前循环播放，播放配置的 `wake_ack.wav` 应答音频，然后接收一条请求；不额外播放叮声。唤醒词和请求连说也可以一次接收。收到请求后节点立即回到待唤醒状态；继续交互需要再次唤醒。唤醒后 120 秒没有检测到语音活动会自动回到待机；检测到语音活动会延长等待时间。该时长可由 `speech_input_node` 的 `inactivity_timeout_sec` 参数调整。

播放期间识别器不处理“停止音乐”这类普通音乐命令；唤醒词本身会先停止循环播放，随后可以在这一轮请求中说“停止音乐”。“闭嘴”会停止当前音乐和语音播放、清空待播音频并取消正在执行的音乐或对话请求，之后需要重新唤醒。语音输出与音乐都经由共享 ALSA `softvol` PCM（底层为 `plug:dmix`）；语音开始播放时音乐会暂停。普通语音播完后不会自动恢复音乐，之后的播放、继续或切歌命令会等待语音结束再执行。

`MusicState.state` 的数值在 `common/include/defination/music_state.hpp` 定义：0 停止、1 搜索中、2 播放、3 暂停、4 错误。音乐状态话题支持保留最后一条消息，语音节点重启后也能获取播放状态。歌曲能否播放取决于开放平台和账号权限，节点只选择平台标记当前账号可播放的搜索结果。
