# ROS 节点代码结构与编写规范

本文是 `xiaocai_robot` 项目所有 ROS 节点的统一规范，适用于 `audio_io_node`、`speech_input_node`、`intent_node`、`dialogue_node`、`memory_node`、`tts_node` 和 `music_node`。规范以 `intent_node` 当前拆分出的节点类与业务逻辑类为结构参考。

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
- 构造函数按需调用初始化方法，顺序统一为 `InitPublishers()`、`InitSubscribers()`、`InitClients()`、`InitServers()`；没有该类对象时跳过对应调用。
- 不声明空的 `Init...()` 方法；节点首次需要某种通信对象时，再增加方法并从构造函数调用。
- 定时器等其他 ROS 资源如需统一初始化，应使用单独、职责明确的 `Init...()` 方法，不与上述四类通信对象混在一起。

## 命名与格式

- 目录、文件名、普通函数、普通方法和局部变量使用 `snake_case`。
- ROS 节点类名使用 `PascalCase` 并以 `Node` 结尾，例如 `MusicNode`、`SpeechInputNode`、`IntentNode`；业务辅助类和结构体使用体现职责的 `PascalCase`，例如 `IntentRules`、`IntentResult`。
- ROS 通信初始化方法按约定使用 `InitPublishers()`、`InitSubscribers()`、`InitClients()`、`InitServers()`；其他方法使用 `snake_case`。
- 类成员变量使用 `snake_case` 并以下划线结尾，例如 `intent_rules_`、`intent_publisher_`。
- 常量使用 `k` 前缀和描述性名称，例如 `kMaxRetryCount`；私有实现细节和文件内常量放在 `.cpp` 的匿名命名空间中。
- 使用两个空格缩进，左花括号与函数或控制语句同行。简单提前返回可以单行书写；多步操作或多分支逻辑使用多行和花括号。
- 头文件使用 `#pragma once`，只包含声明本身需要的依赖；函数实现放在 `.cpp` 文件中。
- 每个头文件应可独立包含，不依赖其他头文件的偶然传递包含。
- 不使用全局 `using namespace`；标准库类型使用 `std::` 限定。

## 配置文件约定

- 每个节点包将本节点配置放在 `<node_package>/config/biz_config.toml`，并通过 `CMakeLists.txt` 安装到包的 `share/<node_package>/config/` 目录。
- 使用 `toml++` 解析 TOML。配置类在节点源码中单独定义，例如 `IntentConfig`；配置类负责定位本节点安装后的 `biz_config.toml`，不在 ROS 节点构造函数或回调中直接解析 TOML。
- 公共配置读取由 `xc::NodeConfigBase`（位于 `common` 公共代码中）负责。节点配置类继承该基类并传入配置文件路径；基类统一读取 `[logging]`，子类只读取本节点专属配置，不能重复实现日志配置解析。
- 节点通过成员对象持有自己的配置类，并将基类解析好的 `logging()` 配置传给 `xc::LogManager`。ROS 节点类本身不继承配置基类。
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

- 节点统一使用 `xc::LogManager` 创建 spdlog logger；节点只通过 `log_manager_.logger()` 打印日志，不各自创建或配置 spdlog logger。
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
- 节点间通信使用已有的 `xiaocai_msgs` 消息和服务类型；修改接口时，同时检查所有发布者、订阅者、客户端和服务端。
- CMake 目标必须列出实际编译的全部 `.cpp` 文件，并声明使用到的 ROS 依赖；`package.xml` 中同步维护包依赖。
- 项目使用 C++17。CMake 配置应支持生成 `compile_commands.json`，供 VS Code IntelliSense 使用。
- 新增 ROS 依赖时，同时更新 `CMakeLists.txt` 和 `package.xml`，并确认交叉编译 sysroot 中包含相应依赖。

## 变更检查

- 新增或修改业务组件时，检查正常输入、无效输入、缺失输入、边界值和错误路径；适合独立验证的组件应提供对应测试。
- 修改话题、服务、消息字段或意图名称时，检查所有上下游消费者和完整节点链路。
- 新增源文件后检查 CMake 目标；更改依赖后检查 `package.xml` 和交叉编译配置。
- 修改后确认构建配置和 VS Code 使用的 `compile_commands.json` 对应当前源文件及 include 路径。
