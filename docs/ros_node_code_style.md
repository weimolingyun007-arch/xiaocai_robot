# ROS 节点代码结构与编写规范

本文是 `xiaocai_robot` 项目所有 ROS 节点的统一规范，适用于 `audio_io_node`、`speech_input_node`、`intent_node`、`dialogue_node`、`memory_node`、`tts_node`、`music_node` 和 `display_node`。规范以 `intent_node` 当前拆分出的节点类与业务逻辑类为结构参考。

## 目录结构

每个节点包按职责组织源码：

```text
<node_package>/
├── include/<node_package>/
│   ├── <node_package>_node.hpp
│   └── <helper_class>.hpp       # 有独立业务组件时添加
├── src/
│   ├── main.cpp                 # ROS 程序入口
│   ├── <node_package>_node.cpp # ROS 节点实现
│   └── <helper_class>.cpp       # 独立业务组件实现
├── config/
│   └── biz_config.toml          # 节点业务及公共配置
├── CMakeLists.txt
└── package.xml
```

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 初始化 ROS，创建并运行节点，最后关闭 ROS；不放业务规则或通信对象创建逻辑。 |
| `src/<node_package>_node.cpp` | 实现 `rclcpp::Node` 子类、ROS 通信初始化和回调协调。 |
| `include/<node_package>/<node_package>_node.hpp` | 节点类声明、ROS 通信成员及必要的回调声明。 |
| `src/<helper_class>.cpp`、`include/<node_package>/<helper_class>.hpp` | 实现可独立组织的业务逻辑，例如意图分类、参数解析或设备控制策略。 |
| `config/biz_config.toml` | 保存本节点的公共配置和业务配置，随节点包安装。 |
| `CMakeLists.txt` | 声明 C++ 标准、依赖、可执行文件源文件和安装规则。 |
| `package.xml` | 声明 ROS 包依赖和包元信息。 |

节点类头文件统一命名为 `<node_package>_node.hpp`，节点实现源文件统一命名为 `<node_package>_node.cpp`，例如 `memory_node/include/memory_node/memory_node.hpp` 和 `memory_node/src/memory_node.cpp`。ROS 节点类统一使用由包名转换得到的 PascalCase 名称并加 `Node` 后缀，例如 `MusicNode`、`SpeechInputNode`；普通业务辅助类按职责命名，不添加 `Node` 后缀。

一个节点包新增 `.cpp` 文件时，必须把它加入对应 `add_executable`。现有节点可以逐步迁移到此布局；迁移时保持话题、服务和消息行为兼容。

## 类的职责边界

### ROS 节点类

- 继承 `rclcpp::Node`，负责节点生命周期、通信对象、ROS 回调和组件间协调。
- 构造函数设置节点名，并调用本类实际需要的通信初始化方法。
- 回调负责接收消息、调用业务逻辑、处理结果并发布或响应；不要在回调里临时创建 ROS 通信对象。
- 不把复杂业务逻辑、状态策略或设备控制逻辑堆进 ROS 回调。将可独立理解和维护的逻辑放到 helper 类中。

### 业务逻辑类

- 封装一个清楚的业务职责，例如 `IntentRules`。
- 尽可能使用普通 C++ 数据类型作为输入和输出，不依赖 ROS 节点、executor 或话题状态。
- 不直接发布消息、调用服务或执行其他 ROS 副作用；由节点类负责将业务结果接入 ROS。
- 公开接口应表达完整输入和输出；结果结构体为各字段提供明确的默认值。

### 程序入口

- `main.cpp` 只负责 `rclcpp::init`、创建节点、`rclcpp::spin` 和 `rclcpp::shutdown`。
- `main.cpp` 不实现业务逻辑、回调或 ROS 通信对象初始化。

## ROS 通信对象初始化

发布者、订阅者、客户端和服务端都必须在对应的 `Init...()` 方法中创建。构造函数和业务回调不得直接调用 `create_publisher`、`create_subscription`、`create_client` 或 `create_service`。

| 初始化方法 | 允许创建的对象 |
| --- | --- |
| `InitPublishers()` | Publisher |
| `InitSubscribers()` | Subscription |
| `InitClients()` | Service Client |
| `InitServers()` | Service Server |

- 每个 `Init...()` 方法只创建对应类型的对象，并赋值给节点类成员。
- `InitPublishers()` 和 `InitSubscribers()` 必须使用局部计数器统计成功创建的对象，并在函数末尾通过 `LOG_INFO` 打印数量，格式统一为 `LOG_INFO("Initialized {} publishers", initialized_count)` 和 `LOG_INFO("Initialized {} subscribers", initialized_count)`。每创建一个对象后递增计数器，确保日志反映实际创建数量。
- 构造函数按需调用初始化方法，顺序统一为 `InitPublishers()`、`InitSubscribers()`、`InitClients()`、`InitServers()`；没有该类对象时跳过对应调用。
- 不声明空的 `Init...()` 方法；节点首次需要某种通信对象时，再增加方法并从构造函数调用。
- 定时器和业务组件等其他资源如需统一初始化，应使用单独、职责明确的 `Init...()` 方法，例如 `InitTimers()`、`InitController()`；不与上述四类通信对象混在一起。

## 命名空间

- 节点类、配置类和业务组件直接放在 `xiaocai_robot` 命名空间内，不使用 `xc` 子命名空间；节点类在全局作用域的引用写作 `xiaocai_robot::MusicNode`。
- 跨节点共享的意图、状态和操作枚举及 MPV 辅助函数保留在 `xiaocai_robot::common`，枚举定义放在 `common/include/defination/` 下的独立头文件中。ROS 消息只通过 `uint8` 等基础字段承载枚举值，不在 `.msg` 里重复定义枚举常量；生产者和消费者在 ROS 消息边界处显式转换为公共枚举。`main()` 按 C++ 入口要求留在全局命名空间；第三方库和 ROS 生成代码保持其原有命名空间。

## 命名与格式

- 目录、文件名、变量使用 `snake_case`。
- 项目自有函数和类成员方法统一使用 `PascalCase`（大驼峰），例如 `OnTextCallback()`、`InitPublishers()`、`Classify()`；函数声明、定义和所有调用点保持一致。`main` 等语言入口、外部接口要求固定名称的回调/重写方法，以及第三方库 API 保持其规定名称。
- ROS 节点类名使用 `PascalCase` 并以 `Node` 结尾，例如 `MusicNode`、`SpeechInputNode`、`IntentNode`；业务辅助类和结构体使用体现职责的 `PascalCase`，例如 `IntentRules`、`IntentResult`。
- ROS 通信初始化方法按约定使用 `InitPublishers()`、`InitSubscribers()`、`InitClients()`、`InitServers()`。
- 订阅者回调统一由节点成员方法处理，方法名采用 `On<消息或主题>Callback` 格式，例如 `OnTextCallback()`、`OnIntentCallback()`、`OnAudioPlayingCallback()`。`InitSubscribers()` 中的订阅 lambda 只转交消息，不承载业务处理逻辑。
- ROS 通信对象成员变量以用途或主题命名，并使用完整对象类型后缀：发布者为 `_publisher_`、订阅者为 `_subscriber_`、客户端为 `_client_`、服务端为 `_server_`。例如 `state_publisher_`、`intent_subscriber_`、`volume_get_client_`、`volume_get_server_`；避免使用 `state_`、`intent_sub_` 等无法直接判断对象类型的名称。
- 封装客户端请求的成员函数统一使用 `Client` 前缀，例如 `ClientVolume()`；客户端请求函数单独分组，不与 `Publish...()` 发布函数放在同一组。
- 类头文件中的私有成员函数按职责分组；每组第一条声明上方添加单行注释说明该组职责，不同组之间空一行。ROS 通信方法可按初始化、消息发布、订阅回调等职责分组。
- `.cpp` 中成员函数定义的分组顺序及组内顺序必须与头文件中的声明一致。
- 类成员变量使用 `snake_case` 并以下划线结尾，例如 `intent_rules_`、`intent_publisher_`。
- 运行状态和资源句柄等类成员需要显式设置初始值时，在 `.cpp` 构造函数的初始化列表中设置；头文件的成员声明处不使用类内初始化器，例如不要写 `VoskRecognizer *wake_recognizer_ = nullptr;`。配置结构体和结果值类型可按需要定义字段默认值。依赖其他资源或运行时 API 创建的对象，在构造函数体中按依赖顺序创建并检查结果。
- 常量使用 `k` 前缀和描述性名称，例如 `kMaxRetryCount`；私有实现细节和文件内常量放在 `.cpp` 的匿名命名空间中。
- 使用仓库根目录 `.clang-format` 规定的格式：基于 LLVM 风格、两个空格缩进；左花括号与函数或控制语句同行。简单提前返回可以单行书写；多步操作或多分支逻辑使用多行和花括号。
- 头文件使用 `#pragma once`，只包含声明本身需要的依赖；函数实现放在 `.cpp` 文件中。
- 每个头文件应可独立包含，不依赖其他头文件的偶然传递包含。
- 不使用全局 `using namespace`；标准库类型使用 `std::` 限定。

## 配置文件约定

- 每个节点包将本节点配置放在 `<node_package>/config/biz_config.toml`，并通过 `CMakeLists.txt` 安装到包的 `share/<node_package>/config/` 目录。
- 使用 `toml++` 解析 TOML。节点负责持有并加载本节点配置，例如 `MusicNode` 持有 `MusicConfig` 并在初始化时读取设置；节点专属配置解析可由对应的 `<node>_config.hpp/.cpp` 实现，配置读取不得放进控制器等业务辅助类，也不得放在 ROS 回调中。
- 可调整的节点业务参数（如程序路径、音频设备、默认关键词和资源路径）放在本节点的 `biz_config.toml` 专属表中；设置结构体中的初始值只作为配置缺失或无效时的回退值，节点将解析后的设置传给控制器。
- 公共配置读取由 `NodeConfigBase`（位于 `common` 公共代码中）负责。节点配置类继承该基类并传入 ROS 包名；基类统一解析 `XIAOCAI_CONFIG_ROOT/<package>/biz_config.toml` 或安装目录中的 `share/<package>/config/biz_config.toml`，并读取 `[logging]`。子类只读取本节点专属配置，不能重复实现日志配置或路径解析。需要显式配置路径的工具和测试可使用路径构造重载。
- 节点通过成员对象持有自己的配置类，并将基类解析好的 `Logging()` 配置传给 `LogManager`。节点把解析后的业务设置值传给控制器等辅助类；设置结构体可以定义在配置头文件中并作为值对象传入，但配置文件的加载与解析仍由节点配置类负责。ROS 节点类本身不继承配置基类。
- 配置字段要有明确类型、默认值和取值范围。文件缺失、解析失败或字段值无效时，按代码中的默认值处理，并输出足以定位配置文件和错误原因的信息。

标准日志配置使用 `[logging]` 表：

```toml
[logging]
directory = "/userdata/log/intent_node"
level = "info"
pattern = "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] %v"
console = true
max_file_size_mb = 20
max_files_per_day = 3
retention_days = 7
```

| 字段 | 含义 | 默认值 |
| --- | --- | --- |
| `directory` | 本节点日志根目录；相对路径和 `~` 由日志管理器展开。未配置时使用 `/userdata/log/<node_name>`。 | 空（采用回退目录） |
| `level` | spdlog 日志级别。 | `info` |
| `pattern` | spdlog 日志格式。 | 带日期时间、级别、logger 名称和消息的格式 |
| `console` | 是否同时输出到控制台。文件日志不可用时仍回退到控制台。 | `true` |
| `max_file_size_mb` | 单个日志文件大小上限。 | `20` |
| `max_files_per_day` | 每天保留的文件总数，包含当前文件和轮转文件。 | `3` |
| `retention_days` | 日志保留天数。 | `7` |

## 日志管理约定

- 节点统一使用 `LogManager` 创建 spdlog logger；节点方法通过 `LOG_INFO(...)`、`LOG_WARN(...)`、`LOG_ERROR(...)` 宏打印日志。宏由 `common/log_manager.hpp` 提供，并转发到当前节点的 `log_manager_` 成员；不要在节点代码中直接调用 `log_manager_.Logger()->...`，也不要各自创建或配置 spdlog logger。
- 文件直接放在 `directory` 指定的节点日志根目录，不再为日期创建子目录。文件名使用本地时间：`<node_name>_YYYYMMDD_HHMMSS.log`；按大小轮转后使用 `<node_name>_YYYYMMDD_HHMMSS.1.log`、`<node_name>_YYYYMMDD_HHMMSS.2.log` 等后缀。
- 同一自然日内重启节点时，继续使用该日期最新的日志文件组；跨日后创建新的时间戳文件组。
- 默认每个文件不超过 20 MB，每天最多 3 个文件（当前文件加轮转文件），保留最近 7 天。修改这些限制时，以 `biz_config.toml` 中的有效配置为准。
- 日志配置和文件轮转由 `common` 统一实现。需要调整默认值、文件命名或清理策略时，修改公共日志管理实现并同步更新本规范，节点不重复实现这些策略。

## 输入校验与错误处理

- 对 ROS 消息、配置文件及其他外部输入校验类型、范围和必填字段，再执行可能产生副作用的操作。
- 默认值、错误路径和缺参行为应显式定义，不能依赖未初始化字段或隐式转换。
- 错误日志应包含足以定位问题的上下文；避免在高频回调中无控制地重复打印同一错误。

## ROS 接口与构建

- 项目机器人话题和服务统一使用 `/xiaocai_robot/` 前缀。
- `xiaocai_msgs` 中每个 `.msg` 的第一个字段必须是 `builtin_interfaces/Time timestamp`，周期状态快照 `BleStatus` 除外。消息发布者在发布时用本节点 ROS 时钟填写 `timestamp`（`now().to_msg()`），不得留默认零值。
- 每个 `.srv` 请求部分（`---` 之前）的第一个字段必须是 `builtin_interfaces/Time timestamp`。客户端发送请求时用本节点 ROS 时钟填写；响应字段按业务需要定义，不受该请求字段顺序要求影响。
- 意图、状态和操作等跨节点枚举统一定义在 `common/include/defination/` 的 C++ `enum class` 中。`.msg` 仅用基础整数类型承载枚举值；新增或调整枚举时同步检查所有生产者、消费者和接口文档。
- ROS 命令消息按业务域拆分，避免用一个通用消息携带互不相关的可选字段。`Intent.msg` 只承载对话分流意图和原始文本；音乐、音量调节、记忆及提醒操作分别使用 `MusicCommand.msg`、`VolumeCommand.msg` 和 `MemoryCommand.msg`。`IntentType` 只表示仍由对话节点处理的闲聊、音量查询、免打扰开关和提醒澄清；其余操作由各自消息对应的 `MusicAction`、`VolumeAction` 或 `MemoryOperation` 表达。这些枚举统一放在 `common/include/defination/`，不在 `.msg` 中声明枚举常量。
- 节点间通信使用已有的 `xiaocai_msgs` 消息和服务类型；修改接口时，同时检查所有发布者、订阅者、客户端和服务端。
- CMake 目标必须列出实际编译的全部 `.cpp` 文件，并声明使用到的 ROS 依赖；`package.xml` 中同步维护包依赖。
- 项目使用 C++17。CMake 配置应支持生成 `compile_commands.json`，供 VS Code IntelliSense 使用。
- 多节点工作区的 `.vscode/c_cpp_properties.json` 应指向 `build/compile_commands.json`；构建脚本合并各节点包生成的编译数据库，确保每个源文件使用本包实际的编译器参数和 include 路径。
- 新增 ROS 依赖时，同时更新 `CMakeLists.txt` 和 `package.xml`，并确认交叉编译 sysroot 中包含相应依赖。

## clang-format Git hook

- 仓库通过 `.githooks/pre-commit` 在提交时格式化已暂存的项目 C/C++ 文件，使用仓库根目录的 `.clang-format`。第三方代码不由此 hook 格式化。
- 如果暂存文件同时含有未暂存修改，hook 会停止提交，避免格式化时把未准备提交的内容一并暂存。先整理该文件的暂存状态，再重新提交。

## 变更检查

- 新增或修改业务组件时，检查正常输入、无效输入、缺失输入、边界值和错误路径；适合独立验证的组件应提供对应测试。
- 修改话题、服务、消息字段或意图名称时，检查所有上下游消费者和完整节点链路。
- 新增源文件后检查 CMake 目标；更改依赖后检查 `package.xml` 和交叉编译配置。
- 修改后确认构建配置和 VS Code 使用的 `compile_commands.json` 对应当前源文件及 include 路径。

## 对话、续听与音乐恢复

- `dialogue_node` 统一决定本轮会话阶段与是否需要用户补充，发布 `ConversationState`。显示、语音输入和音乐节点消费该状态，不各自猜测对话是否结束。
- 机器人追问时，等问题音频播放结束后直接续听，不重复播放唤醒应答。最终回答播完后回到待唤醒状态；有无音乐均使用同一续听逻辑。
- 监听必须有最大时长。首次唤醒默认最多 120 秒；追问后的续听由 `followup_timeout_sec` 配置，默认 15 秒、范围 1–120 秒。背景噪声不能无限延长窗口，超时关闭监听并丢弃旧识别任务。
- 对话语音通过 `TtsRequest.request_id` 与 `TtsResult` 关联。完成指实际 WAV 播放结束；生成 WAV 或唤醒应答播完不代表对话回复完成。播放失败、取消和丢弃也必须反馈结果。
- 音乐中唤醒使用暂停保留播放进度，整轮对话结束且语音播放完成后再恢复。追问期间保持暂停；明确暂停、停止、切歌、重新点歌或“闭嘴”取消旧的自动恢复。原本手动暂停的音乐不能被自动开启。
- 音乐元数据及歌词时间线由 `music_node` 提供，`display_node` 只负责展示和进度对齐；歌词获取在独立工作线程执行，切歌时取消旧结果，不能阻塞 ROS 回调或音乐播放。

## 显示状态与授权提示

- Wi-Fi 信号和连接情况用图标常驻右上角，所有页面均保留，不显示“已连接”等状态文字；已连接按信号强弱点亮弧线，断开或接口不可用显示灰色图标和斜线。接口名放显示节点配置。无线关联/IP 状态与互联网、外部服务可用性分别表达，不能用 Wi-Fi 图标保证第三方 API 可用。
- 收到点歌指令后，搜索和准备播放期间显示思考，实际开始播放后显示歌词。
- 第三方音乐登录失效时，由音乐业务组件生成官方登录链接并发布独立状态，显示节点本地绘制二维码，语音提示用户扫码。登录页优先于表情和歌词；确认授权后退出，二维码到期刷新，网络错误有重试提示。
- 授权确认由用户完成，不能代替用户确认登录。ROS 消息只传登录页需要的链接和状态，不传账号 token、Cookie 或私钥；日志不打印这些凭证。
