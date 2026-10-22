# 小蔡机器人 C++ 节点工程

适用：RK3566 泰山派、Ubuntu 22.04、ROS 2 Humble、RK809 音频小板 A。

## 工程结构

十个 ROS 2 节点目录直接放在工程根目录。每个节点目录包含自己的 `include/`、`src/`、`CMakeLists.txt` 和 `package.xml`。

| 目录 | 职责 |
| --- | --- |
| `audio_io_node` | 咪头采样、喇叭播放与音量控制 |
| `speech_input_node` | SenseVoice 中文语音识别、Vosk 唤醒词检测 |
| `intent_node` | 判断音乐、音量、免打扰、记忆、提醒和闲聊意图 |
| `dialogue_node` | 本地功能协调、DeepSeek 对话及本地工具调用 |
| `memory_node` | SQLite 记忆、免打扰状态和提醒 |
| `tts_node` | sherpa-onnx 离线中文语音合成 |
| `music_node` | 调用网易云 ncm-cli 搜歌、播放及控制 |
| `wifi_manager` | 扫描和连接 Wi-Fi、自动重连并发布连接状态及上下行速率 |
| `ble_manager` | 管理蓝牙低功耗扫描、连接、GATT 写入与通知接收，并发布蓝牙状态 |
| `display_node` | 横屏开机就绪页、表情、音乐信息和同步歌词显示 |
| `common` | 节点共享的配置基类、日志管理、业务枚举和 MPV 辅助函数 |
| `thirdparty` | C++ 头文件和 ARM64 动态库 |
| `xiaocai_msgs` | ROS 2 自定义消息和服务接口源码 |
| `bringup` | 编译、启动、停止和 systemd 开机自启脚本 |
| `docs` | 节点代码结构和编写约定 |
| `build` | 每个节点由 CMake 和 Make 生成的可执行文件 |

源码目录内的模型文件放在使用它们的节点目录下。板端运行时，DeepSeek 密钥和记忆数据库分别放在 `resource/deepseek.env` 和 `resource/memory.sqlite3`，文件权限为 600。首次更新时部署脚本会从原来的 `dialogue_node/.secrets/`、`memory_node/data/` 迁移数据。

### 节点职责与配置

ROS 节点类负责 ROS 通信和生命周期，将复杂业务交给专用组件处理：`speech_input_node` 使用 `SpeechInputController` 管理唤醒、录音和识别流程；`intent_node` 使用本地训练的 `IntentClassifier` 模型分类意图和用户纠偏表达；`dialogue_node` 使用 `DialogueController` 管理对话和模型工具调用；`music_node` 使用 `MusicController` 执行音乐任务；`memory_node` 使用 `MemoryManager` 管理 SQLite 记忆和提醒。各节点的配置解析由对应的配置类完成，共享的 `NodeConfigBase` 负责 TOML 文件定位和日志配置读取。

每个节点的业务配置位于 `<node_package>/config/biz_config.toml`。运行包会将它们放在 `config/<node_package>/biz_config.toml`，启动脚本通过 `XIAOCAI_CONFIG_ROOT` 指向这个配置根目录；未设置该变量时，节点从 ROS 安装的包共享目录读取配置。日志使用 spdlog，由 `common` 中的 `LogManager` 管理。默认写入 `/userdata/log/<node_name>`，单文件上限 20 MB，每天最多 3 个日志文件，保留 7 天；具体值可在节点配置的 `[logging]` 表中调整。

## 编译和运行

工程使用同一个脚本识别构建环境。在工程目录运行 `make build`，会调用 `bringup/ota/build.sh`：

```bash
make build
```

在 RK3566 板端，脚本使用板端 ROS 2 Humble、CMake 和 Make 原生编译；在 x86_64 电脑上，脚本使用 `/opt/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu` 交叉编译。产物都位于 `build/<节点名>/<节点名>`。

电脑端交叉编译还需要 RK3566 的 AArch64 ROS/sysroot，默认位置是 `~/.cache/xiaocai_robot/aarch64-sysroot`；也可以通过 `XIAOCAI_SYSROOT` 指定其他位置。交叉编译产物不会自动部署到板端。运行时需能解析并访问 `api.deepseek.com`。

显示程序需要 X11、Xft/FreeType 和中文字体。Ubuntu 原生编译安装 `libx11-dev libxft-dev libfreetype6-dev fonts-noto-cjk`；交叉编译时应将对应 ARM64 开发包的头文件及库同步到 sysroot，包含 `X11/Xft/Xft.h`、`X11/extensions/Xrender.h`、`fontconfig/fontconfig.h` 和未带版本号的开发库链接。板端运行需要 `libxft2`、`libfontconfig1`、`libfreetype6` 和 `fonts-noto-cjk`；字体由显示配置 `font_family` 选择。

在板端编译完成后启动节点：

```bash
bash bringup/scripts/start_all_node.sh
```

语音输入节点在待机时只监听唤醒词。唤醒后接收用户请求，处理及播报期间暂停接收普通语音，旧识别结果不会成为下一轮请求。机器人主动追问或要求补充参数时，问题播完后直接继续监听，不需要再次唤醒或重复播放应答音；回答完成且无需追问时回到待唤醒状态。“闭嘴”立即停止回复和音乐，并取消这轮对话。

监听有明确上限：首次唤醒窗口由 `inactivity_timeout_sec` 控制，默认最多 120 秒；机器人追问后的续听窗口由 `[speech_input].followup_timeout_sec` 控制，默认最多 15 秒，可配置 1–120 秒。窗口从开始监听时计时，背景噪声不会无限续期；超时未收到有效回答则关闭监听，等待下次唤醒。音乐中唤醒会暂停并保留播放位置，整轮对话结束（包括追问超时）且语音播完后自动恢复；用户明确暂停、停止音乐或说“闭嘴”会取消自动恢复。

## DeepSeek 调用板端工具

普通聊天和复杂请求由 `dialogue_node` 发送给 DeepSeek，并携带本地工具声明。模型返回 `tool_calls` 后，C++ 程序校验参数并执行对应功能，把真实结果以 `role: tool` 和 `tool_call_id` 回传，再请求模型生成最终回答。这是 [DeepSeek 官方 Tool Calls 协议](https://api-docs.deepseek.com/guides/tool_calls/)；模型不会自己登录板子或执行 shell。

| 工具名 | 本地执行方式 |
| --- | --- |
| `get_current_time` | C++ 读取板端系统时钟，返回北京时间、日期、星期；准确性依赖板端时钟 |
| `get_volume` | 读取对话节点缓存的 `/xiaocai_robot/audio/volume/state` 最新值 |
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

板端查询后逐条回传结果，比如 `{"ok":true,"percent":120}`，模型再结合实测时间和音量回答。简单音量、音乐、记忆和提醒命令由本地训练的意图模型分类后直接执行；组合请求交给对话模型处理，避免只执行其中一项。目前没有实时天气查询工具。

HTTP 和工具等待使用独立工作线程，ROS 主线程继续处理服务反馈。“闭嘴”和新的唤醒会取消旧的云端任务；一次请求后的自动休眠不会取消该任务。最多 4 轮工具执行、8 次工具调用；对参数和工具名称进行本地校验，不提供任意 shell 工具。同一 `tool_call_id` 不会重复执行。超时可能已经产生效果的修改操作不会由程序自动重试。

成功播放或继续音乐后，若无需追问，歌曲作为声音反馈，不再朗读模型最终确认，避免 TTS 把刚开始的音乐暂停。模型仍收到工具结果并生成最终确认；播放失败由对话节点根据错误回复。`MusicCommand` 和 `MusicState` 的 `request_id` 用于匹配工具请求，普通本地命令为 0。

回归测试源码位于 `dialogue_node/tests/`：模拟 DeepSeek HTTP 协议，但使用真实 ROS 执行器和消息/服务验证工具执行、错误参数、重复调用、音乐反馈关联及取消。可在已配置的 CMake 构建目录启用 `-DXIAOCAI_BUILD_TOOL_TESTS=ON`，构建 `dialogue_tool_calls_test`；测试要在隔离 ROS_DOMAIN_ID 下运行，启动 `mock_deepseek.py`，并把测试节点的 `api_url` 设为 `http://127.0.0.1:18763`，设置虚拟 `DEEPSEEK_API_KEY`。默认编译和运行包不包含测试程序。

## 打包和部署

在电脑端进入工程目录后运行：

```bash
make build
make ota
make deploy
```

`make build` 编译节点。`make ota` 只打包现有编译产物，不会触发编译；它在工程根目录生成 `xiaocai_robot_时间.tar.gz`。压缩包根目录为 `xiaocai_robot/`，其下有 `bin/`、`config/`、`resource/`、`bringup/`、`thirdparty/` 和 `msgs/`。十个可执行文件集中在 `bin/`；十个节点的 `biz_config.toml` 放在 `config/<节点名>/`，启动脚本通过 `XIAOCAI_CONFIG_ROOT` 加载；语音模型和唤醒提示音放在 `resource/speech/`、语音合成模型放在 `resource/tts/`、意图分类模型放在 `resource/intent/`、六种表情素材放在 `resource/display/`；第三方依赖库放在 `thirdparty/`；自定义消息与服务生成的 ROS 2 类型支持库放在 `msgs/`。`bringup/` 包含启停脚本和 systemd 服务单元。接口定义保留在工程源码 `xiaocai_msgs/msg/` 和 `xiaocai_msgs/srv/`，生成的 C++ 头文件在 `build/install/include/`；这些内容不随运行包部署。压缩包不含节点源码、头文件或构建配置。打包校验成功后会删除旧压缩包，只保留最新包。

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
- `/xiaocai_robot/dialogue/command`：交给对话节点处理的意图，类型为 `xiaocai_msgs/msg/DialogueCommand`，只在闲聊、免打扰开关和提醒澄清等对话路径发布
- `/xiaocai_robot/music/command`：音乐操作，类型为 `xiaocai_msgs/msg/MusicCommand`，字段 `action`、`query`、`request_id`
- `/xiaocai_robot/dialogue/state`：`ConversationState`，空闲、监听、处理中、回复中的会话阶段，以及是否等待用户补充
- `/xiaocai_robot/tts/text`：无关联编号的普通合成文字（如音乐错误提示、音量查询结果）
- `/xiaocai_robot/tts/request`：`TtsRequest`，对话回复文字和 `request_id`
- `/xiaocai_robot/tts/result`：`TtsResult`，合成失败或实际播放结束后的关联结果；不能把 WAV 生成成功当成回复播完
- `/xiaocai_robot/audio/result`：`AudioPlaybackResult`，WAV 路径及播放结果；取消、文件缺失或静默丢弃也回报失败
- `/xiaocai_robot/audio/play`：待播放 WAV 路径，类型为 `xiaocai_msgs/msg/AudioPlay`；`path` 为空时停止播放并清空待播队列
- `/xiaocai_robot/audio/startup_music`：`std_msgs/msg/Empty`；显示节点在核心节点全部就绪后发一次，音频节点播放柔和开机旋律，不解除语音待机静音
- `/xiaocai_robot/state`：机器人状态，类型为 `xiaocai_msgs/msg/RobotState`，字段 `state`
- `/xiaocai_robot/system/readiness`：核心节点初始化状态，类型为 `xiaocai_msgs/msg/NodeReadiness`；每个核心节点就绪后每秒发布一次，显示节点用它决定何时结束开机页
- `/xiaocai_robot/memory/command`：记忆操作请求，类型为 `xiaocai_msgs/msg/MemoryCommand`，字段 `operation`、`request_id`、`content`、`delay_seconds`、`enabled`
- `/xiaocai_robot/memory/result`：记忆操作结果，类型为 `xiaocai_msgs/msg/MemoryResult`，字段 `operation`、`request_id`、`ok`、`error`、`facts`、`content`、`deleted`、`enabled`
- `/xiaocai_robot/reminder/due`：到期提醒，类型为 `xiaocai_msgs/msg/Reminder`，字段 `id`、`content`
- `/xiaocai_robot/music/state`：音乐状态，类型为 `xiaocai_msgs/msg/MusicState`，字段 `state`、`song_id`、`title`、`artist`、`position_seconds`、`duration_seconds`、`error`、`request_id`
- `/xiaocai_robot/music/login`：`MusicLogin`，需要授权时携带官方登录链接、加载标记和错误状态，屏幕优先显示二维码；不传递账号 token
- `/xiaocai_robot/music/lyrics`：`MusicLyrics`，歌曲编号、标题、歌手、加载标记、每句开始秒数和文字；保留最近一首的完整时间线
- `/xiaocai_robot/audio/volume/state`：`std_msgs/msg/Int32`，音频节点发布音量变化状态
- `/xiaocai_robot/audio/get_volume`：`xiaocai_msgs/srv/GetVolume`，音量查询时由 `intent_node` 直接请求 `audio_io_node` 的当前音量

`Intent.type` 使用 `uint8` 承载 `xiaocai_robot::common::IntentType` 的枚举值；该枚举包含闲聊、音量查询、免打扰开关和提醒澄清等可路由意图。聊天、免打扰开关和提醒澄清由 `DialogueCommand` 交给对话节点；音量查询由 `intent_node` 直接调用 `audio_io_node` 的 `GetVolume` 服务，并通过 TTS 文本话题播报结果，不经过对话节点。音乐、音量调节、记忆和提醒创建分别通过 `MusicCommand`、`VolumeCommand`、`MemoryCommand` 传递，并使用 `MusicAction`、`VolumeAction`、`MemoryOperation` 表示操作类型；这些枚举定义在 `common/include/defination/`。`MusicCommand.request_id` 与 `MusicState.request_id` 使用 `uint64`；`0` 表示普通语音命令，音乐工具请求使用非零编号并由 `MusicState` 原样回传。除周期发布的 `BleStatus` 状态快照外，自定义消息和服务请求部分的首字段均为 `builtin_interfaces/Time timestamp`，发送时填写本节点 ROS 时钟。

所有机器人话题和服务统一使用 `/xiaocai_robot/` 前缀。对话意图、音乐操作、音量操作和记忆操作使用各自的自定义命令消息，避免把无关业务参数混装在一个通用消息中。状态与操作枚举统一在 `common/include/defination/` 的 C++ 枚举中定义，由消息的整数型字段承载；`.msg` 不声明重复的枚举常量。除 `BleStatus` 外，每个自定义消息以及服务请求部分的首字段都是 `builtin_interfaces/Time timestamp`。自定义接口定义位于 `xiaocai_msgs/msg/` 与 `xiaocai_msgs/srv/`，由 `make build` 先生成，再编译依赖节点。

## 网易云语音点歌

板端需已安装 `/usr/local/bin/ncm-cli`（官方 `@music163/ncm-cli`）及 `mpv`，并在 `lckfb` 用户下配置开放平台凭证和扫码登录。CLI 运行时安装在 `/opt/ncm-cli`，账号配置位于 `/home/lckfb/.config/ncm-cli/`，更新机器人运行包时会保留。机器人依赖的 CLI、mpv 与 ROS 2 一样由板端预先安装。

唤醒后可以说“播放晴天”“放一首歌”“暂停音乐”“继续播放”“停止播放音乐”“下一首”“上一首”。不指定歌名时搜索轻音乐。`intent_node` 发布 `MusicCommand`，用 `MusicAction` 表示动作、`query` 表示歌名或关键词；`music_node` 处理命令，点歌时由音乐节点保存最多 6 首可播放的不同歌曲，按歌名排除翻唱、伴奏等同名版本，并循环播放队列；“下一首”切换到其他歌名。搜索结果只有当前歌曲的版本时，切歌会补充搜索其他音乐。唤醒词会暂停当前播放，整轮对话结束后继续原进度。点歌失败时本地语音提示，不请求 DeepSeek。

音量范围是 0–150，当前配置 `audio_io_node/config/biz_config.toml` 的启动默认值是 80，可通过 `volume_percent` 调整。RK809 DAC 在 100% 已达硬件满量程；100% 以上通过共享 ALSA `softvol` PCM 对语音和音乐一起增加数字增益，最高额外 +6 dB。唤醒应答文件 `wake_ack.wav` 的音频峰值约为 -6.2 dBFS，为最大增益留有余量。唤醒后可通过“音量调低一点”逐步调整，也可指定 150%。

音乐播放期间，`speech_input_node` 不运行 SenseVoice 转写，只用 Vosk 检测唤醒词和“闭嘴”，避免把歌曲识别成对话。叫“小蔡小蔡”或“小蔡同学”会暂停音乐，播放配置的 `wake_ack.wav` 应答音频，再接收请求；不额外播放叮声。机器人有追问时继续交互，音乐保持暂停；最后一句回复实际播放完毕或续听超时后，恢复原歌和播放位置。原本由用户暂停的音乐不会因对话结束而被自动开启。

播放期间识别器不处理“停止音乐”等普通音乐指令，需先唤醒后发出指令。明确暂停、停止、切歌或重新点歌会接管当前播放任务并取消旧的自动恢复。“闭嘴”会停止音乐和语音、清空待播音频，取消搜索与云端任务，之后需要重新唤醒。语音和音乐均经共享 ALSA `softvol` PCM（底层为 `plug:dmix`）。

`MusicState.state` 的数值在 `common/include/defination/music_state.hpp` 定义：0 停止、1 搜索中、2 播放、3 暂停、4 错误。音乐状态话题支持保留最后一条消息，语音节点重启后也能获取播放状态。歌曲能否播放取决于开放平台和账号权限，节点只选择平台标记当前账号可播放的搜索结果。

`display_node` 先启动并显示与待机表情同款的极简动画开机页。音频、语音识别、意图、对话、记忆、TTS 和音乐节点完成本地资源初始化后，在 `/xiaocai_robot/system/readiness` 定期发布就绪状态；所有核心节点就绪后，音频节点播放一次柔和开机旋律，显示页切入机器人当前表情或歌词页。开机旋律不会解除语音待机静音，也不会把机器人状态切成“说话”。点击歌词页的下一首或上一首时，切歌搜索期间保持歌词页；用户唤醒后仍优先显示聆听和对话表情。Wi-Fi 和蓝牙连接不阻塞开机。`dialogue_node` 根据唤醒、对话阶段和实际语音播放决定机器人状态并发布 `/xiaocai_robot/state`；`display_node` 在 `ShowRobotState()` 中将 `RobotState` 映射为待机、聆听、思考、说话、免打扰和静音六种表情。表情素材位于 `resource/display/`；启动脚本把 DSI 输出旋转为 800×480 横屏。状态话题使用 transient-local QoS，显示节点可读取最近状态。

### 同步歌词与会话完成

播放或暂停音乐时，`display_node` 显示纯歌词页：标题、歌手、上一句、青色当前句、下一句以及进度和时长；切换曲目期间保留当前音乐页，等新曲目载入后刷新。音乐播放状态不受机器人表情状态打断，停止或播放错误时再回到表情页。页面底部三个按钮按整个屏幕宽度等距对称排列。用户在右侧触摸并滑动时，临时显示悬浮的 0–150% 音量控件，不占用音乐页面布局；松开后短暂显示音量再自动隐藏。滑条直接向音频节点发静音确认命令，不播报“音量调到百分之……”；语音指令和对话工具继续使用原有确认流程。启动脚本会在屏幕旋转后把触摸设备映射到 DSI 输出，令触摸和鼠标使用同一屏幕坐标。

`MusicController` 保存播放队列、歌曲编号及当前索引；手动切歌和自然播完使用同一条加载路径，确认音源已改变且音频输出就绪后才发布新歌曲及歌词。网易云 CLI 通常只在 MPV 中加载当前一首，因此不使用 MPV `playlist-pos` 推断网易云队列位置。独立的 `MusicLyricsLoader` 通过 `ncm-cli song lyric --songId ... --output json` 获取 LRC，支持毫秒时间戳、多时间标签和 offset。歌词请求有超时和缓存，切歌后丢弃旧请求结果，网络等待不阻塞播放。`LyricsPresenter` 按实际播放秒数选择当前行，暂停时冻结；无时间歌词显示“暂无歌词”，开头及空白段显示“音乐前奏/音乐间奏”。

模型最终回答使用 `{"reply":"要说的话","expect_reply":true或false}`。需要用户确认或补充时设置 `expect_reply=true`；程序只朗读 `reply`。本地缺参追问也使用相同续听策略。`DialogueController` 用 TTS 请求编号跟踪回复，只有 `audio_io_node` 报告对应 WAV 播放结束后，才继续监听或结束对话；唤醒应答、旧回复和被取消的请求不会提前结束当前对话。续听不依赖是否正在播放音乐。

### Wi-Fi 常驻状态与网易云扫码登录

`wifi_manager` 通过 NetworkManager 扫描网络、连接指定 SSID，并为保存的网络开启自动重连；断线时也会定期尝试恢复连接。`/xiaocai_robot/wifi/get_list` 返回网络名称、信号百分比和是否加密，`/xiaocai_robot/wifi/connect` 负责连接，`/xiaocai_robot/wifi/get_status` 查询状态。Wi-Fi 密码通过服务请求的标准输入传给 `nmcli`，不会写入节点日志。节点每秒发布 `/xiaocai_robot/wifi/status`，包含连接状态、信号强度、IP 地址和上下行速率。

`ble_manager` 通过板端 BlueZ 的 `bluetoothctl` 管理 BLE：`/xiaocai_robot/ble/scan` 扫描附近设备并返回 `BleScanList`，列表项 `BleScanResult` 含地址、名称和可选 RSSI；`/xiaocai_robot/ble/connect` 服务按设备地址连接外设。主动向设备发送数据使用 `/xiaocai_robot/ble/send` 服务，将字节写入指定 GATT 特征并返回操作结果；外设异步通知的数据发布到 `/xiaocai_robot/ble/received` 话题。`/xiaocai_robot/ble/notify` 服务开关特征通知。`/xiaocai_robot/ble/get_status` 可查询状态，节点每两秒发布 `/xiaocai_robot/ble/status`。`BleStatus.state` 由 `common/include/defination/ble_status.hpp` 定义：`0 Disconnected` 表示没有连接或扫描（适配器不可用、关闭或空闲也归此值），`1 Scanning` 表示扫描中，`2 Connected` 表示已有设备连接；扫描与连接同时发生时优先报告 `Connected`。状态不携带采样时间；连接地址和名称只在 `Connected` 状态下有效。扫描时长为 1–30 秒。GATT 特征参数可传 UUID 或 BlueZ 显示的属性句柄。当前后端由板端主动连接 BLE 外设并接收其通知；App 主动连接板端并写入消息需要本地 GATT 服务端能力，目前尚未实现。板端需运行 BlueZ 服务并提供 `bluetoothctl`。显示节点将蓝牙图标放在状态栏最右侧，Wi-Fi 图标紧邻其左侧。

`display_node` 订阅 Wi-Fi 和蓝牙状态消息，顶部状态栏在表情、歌词和登录页面都显示左侧系统日期和时间（`YYYY-MM-DD  HH:MM`，日期与时间之间两个空格），右侧显示 Wi-Fi 下行/上行速率和连接图标。速率按 B/s、K/s 或 M/s 显示；接口未连接或暂时无法读取计数时显示 `--`。连接后 Wi-Fi 图标按信号强弱点亮青色弧线，未连接或接口不可用时显示灰色图标和斜线。这个图标表示无线连接，不等于网易云接口必然可用。

收到音乐播放指令后，在搜索和准备播放期间显示思考表情；播放就绪后切到歌词页。搜索失败日志记录 CLI 退出码、超时和接口状态，便于区分登录失效与请求失败。

`ncm-cli 0.1.7` 的播放后台请求固定在 3 秒后超时，板端的公网 IP 查询、歌曲地址请求和 MPV 冷启动可能超过这个时长。启动脚本通过 `NODE_OPTIONS` 预加载 `resource/music/ncm_player_compat.cjs`，只对该包的 0.1.7 版本把 3 秒计时调整为 20 秒；未修改已安装的 CLI。升级 CLI 后应重新评估并移除此适配。音乐节点保持后台订阅，每次最多等待 25 秒，确认 MPV 的音频输出完成初始化后才报告播放成功，失败最多重试一次。首曲就绪后立即播放，后续曲目保存在节点内存中，切歌时再获取目标音源；不再依靠可能被唤醒中断的 CLI 队列追加，也不使用单曲循环兜底。后台和 MPV 必须继承启动脚本的 `ALSA_CONFIG_PATH`，否则独立启动的后台可能找不到 `xiaocai_speaker`。

`MusicLoginManager` 独立检查网易云登录状态。正常时每 60 秒检查；需要授权时调用官方 `ncm-cli login --background`，将登录链接发布到保留的 `/music/login` 话题，并语音提示扫码。`display_node` 使用 `thirdparty/qrcodegen-1.8.0` 中的 [MIT 二维码生成库](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/cpp) 在本地画码，保留白色留边；登录页优先于表情与歌词。等待授权时每 3 秒检查，二维码每 120 秒更新；临时网络失败显示重试提示，15 秒后重试，不把网络异常直接当成退出登录。确认登录后自动关闭二维码页。

登录和授权必须由用户在网易云音乐 App 中确认。CLI 凭证仍保存在板端原位置，机器人消息、日志和运行包不包含 token 或私钥。登录提示生成时仍遵循现有静音规则；已执行“闭嘴”或处于启动待机时，不强制解除音频静默。
