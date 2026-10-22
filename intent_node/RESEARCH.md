# RK3566 本地中文意图识别调研

首轮调研日期：2026-10-11。该轮范围为资料核对、现有代码和板端资源检查；当时未安装候选模型、训练模型或修改运行代码。

> 当前实现已于 2026-10-16 更新：`IntentNode` 使用训练得到的字符 n-gram 分类模型判断意图，并由模型头判断用户是否在纠偏；运行路径不再使用关键词或正则决定意图。下文首轮候选分析保留为历史记录，不能视为当前运行架构。

## 现状与问题

`intent_node` 逐句使用关键词和正则分配意图，默认进入 `chat`，没有保存等待歌名、等待时间、等待确认等对话状态。

板端日志显示：“两只老虎”进入 `chat`；“播放两只老虎”进入 `music_play` 并成功播放。日志不能判断用户是否只说了歌名，或 ASR 漏掉了“播放”。之前网易云控制端错误属于执行层故障，已经单独修复，不能通过意图模型修复。

本次资源快照：AArch64，MemTotal 1957 MiB，MemAvailable 406 MiB，无 swap；speech_input_node RSS 约 634 MiB，tts_node RSS 约 215 MiB。快照随负载变化，不代表峰值。项目已有 AArch64 `thirdparty/onnxruntime/libonnxruntime.so`，后续接入需要匹配版本的 C/C++ 头文件和中文 tokenizer。

## 候选方案

| 方案 | 文件大小参考 | 接入方法 | 数据要求与限制 |
|---|---|---|---|
| fastText 监督分类 | 由词表、向量维度、bucket 和量化决定；没有统一的机器人模型大小 | C++ 原生库，CPU 推理 | 需要机器人意图标注数据；中文须统一分词或按字切分；可作为资源和准确率基线 |
| 中文 RoBERTa Tiny：uer/chinese_roberta_L-2_H-128 | 发布的 PyTorch 权重约 12.8 MB | 主机微调分类头、导出 ONNX，板端 C++ 推理 | 2 层、隐藏维度 128；发布模型是预训练模型，需要微调；实际大小随导出和分类头变化 |
| BAAI/bge-small-zh-v1.5 | 一份 FP32 权重约 95.8 MB，24M 参数 | 中文文本向量加带标签示例检索，或训练分类头；导出 ONNX | 可以先用意图示例构建对照；语义相似不等于意图一致，否定、讨论与执行必须单独评估 |
| Qwen2.5-0.5B-Instruct，GGUF Q4_K_M | 官方文件约 491 MB | llama.cpp C/C++，输出受约束 JSON | 可输入对话历史、生成意图和参数；权重外还需缓存与工作内存；当前资源余量下不作为首选 |

以上是文件大小，不是运行内存。不同框架版本文件不需要全部下载；例如 BGE 的 `.bin` 和 `.safetensors` 是同一模型的不同存储格式。候选的板端延迟、峰值内存和机器人任务准确率均未实测，不能由模型名称或通用榜单推断。

## NPU 和量化

瑞芯微 RKNN-Toolkit2 支持 RK3566；RKLLM 当前官方支持列表列出 RK3588、RK3576、RK3562、RV1126B，没有 RK3566。不能因板子有 NPU 就假设官方 RKLLM 可用于这块板。

先做 CPU 基线。编码器是否能在 RK3566 NPU 上完整运行，还要检查具体模型算子、导出形状、量化和驱动。ONNX Runtime 文档建议 Transformer 优先考虑动态量化，但实际性能和精度必须对照未量化模型验证。

## 首轮调研时的推荐路线（历史记录）

在决定部署分类器前，优先验证 **DeepSeek 工具调用与现有本地规则结合** 是否已经满足需求。2026-10-11 已在 `dialogue_node` 实现 tools 声明、tool_calls 执行循环及真实结果回传，支持时间、音量、音乐、记忆和提醒；简单命令仍走本地规则，组合请求交给 DeepSeek。DeepSeek 官方支持工具调用，但函数需由客户端实现和执行，云端不会直接执行板端命令。

例如声明 `get_current_time`，模型返回调用请求，板端读取系统时间（明确时区），再以 tool 消息回传结果供模型组织回答；无需开放板端公网接口。天气需要对应数据源，音乐可通过既有节点执行。本地明确控制保留直接执行路径，避免每条简单指令都经历云端往返。板端时钟还需确认已同步。

若实测仍需离线意图理解、减少联网依赖或降低云端往返延迟，再做 **fastText 基线与中文 RoBERTa Tiny 分类器的同集比较**。BGE-small 作为不微调的语义匹配对照；目前不把生成式 Qwen 常驻板端。

在现有 `intent_node` 内组合：

1. 对话状态：保存正在等待的参数，收到下一句话后补全；有超时、取消和唤醒/休眠重置机制。
2. 明确规则：闭嘴、停止、取消和明确数值等控制优先处理；否定句需要独立规则和训练样本。
3. 意图模型：对未命中规则的完整表达分类，输出类别和评分。
4. 参数提取：歌名、音量、提醒内容和时间分别提取；分类模型本身不自动提供这些字段。
5. 判断不明确时：根据待补参数追问，或交给现有对话节点；模型评分不直接当作准确概率。

例：“放首歌”识别为播放但缺歌名，机器人询问后设置 `waiting_song`；下一句“两只老虎”补入 `MusicCommand.query`。孤立的“两只老虎”没有相同上下文时不能一律触发播放。若采用默认播放策略，应避免同时声称正在等待用户指定歌曲。

分类结果按业务类型映射到独立消息：对话请求使用 `DialogueCommand` 和 `/xiaocai_robot/dialogue/command`，音乐操作使用 `MusicCommand`，音量设置使用 `VolumeCommand`，记忆和提醒操作使用 `MemoryCommand`。音量查询由 `intent_node` 直接调用 `audio_io_node` 的 `GetVolume` 服务，随后经 TTS 文本话题播报，不经过 `dialogue_node`。其余需要对话能力的意图才发送给 `dialogue_node`。这让分类器输出保持类型化，执行节点仍通过明确的操作字段和参数接收命令；不增加 ROS 节点。

## 实验设计

- 先覆盖当前音乐、音量、提醒、记忆、免打扰和聊天类别；每类先整理 50–100 条多样表达作为起点，再依据错误补样本，不保证这个数量足够。
- 加入 ASR 实际转写、否定句、相似意图、参数缺失、未知请求和聊天负例。例如“播放两只老虎”“不要播放两只老虎”“两只老虎是什么歌”不能合并处理。
- 保留独立测试集；同一语句模板、仅替换歌名的近似样本不要跨训练/测试集，以免高估泛化能力。
- 分别评估：意图每类 Precision/Recall、Macro-F1、参数提取、多轮补全、未知意图拒绝；再验证麦克风到动作的完整链路。
- 在 ASR/TTS 同时运行的真实板端负载下测 p50/p95 延迟、峰值 RSS 和 CPU 占用。初期目标为新增 RSS 不超过 100 MiB、意图阶段 p95 不超过 200 ms；这是验收目标，不是测量结果。
- 依据验证集选择评分阈值和前两类别分差；不能直接使用固定相似度 0.8 或分类评分 0.9 就声称可靠。

## 首轮 RoBERTa 实验（历史记录）

早期曾在 `intent_node/training/` 进行 `hfl/rbt3` 微调、评估、ONNX 导出及动态 INT8 量化实验。360 条种子语料每类 20 条（14 train、3 validation、3 test），均为人工示例，没有板端 ASR 真实转写。实验在开发机临时虚拟环境完成；其 checkpoint 和 ONNX 导出文件已清理。

8 轮微调的最佳验证结果在第 6 轮：accuracy 0.593、Macro-F1 0.560；独立测试集 accuracy 0.500、Macro-F1 0.490。“放音乐”被误分为 `music_pause`，最高分只有 0.126。语料量和泛化效果都不足以接入实时命令。绝对时间闹钟能力尚不存在，相关输入暂归入提醒澄清类。

生成的 FP32 ONNX 约 147 MiB，动态 INT8 约 85 MiB。该实验模型没有接入 ROS 节点；当前节点改用本地轻量分类器，避免板端依赖这套 ONNX tokenizer/runtime。

## 当前运行模型（2026-10-16）

已训练 `intent_node/assets/intent_model.json`，采用字符 n-gram softmax 分类，包含意图分类与纠偏检测两个模型头。C++ `IntentClassifier` 在板端 CPU 直接读取模型文件；动作类别由模型预测，后续代码只将类别映射到 ROS 消息，并抽取/校验歌名、音量、提醒等参数。

训练集由原有 360 条种子语句、人工整理的纠偏样本和纠偏表达硬负例组成。最新模型的 validation intent accuracy / Macro-F1 为 0.819 / 0.818，test 为 0.806 / 0.800；纠偏检测在 validation 的 Precision/Recall 为 1.000 / 1.000，在 test 为 1.000 / 0.833。纠偏测试样本数量仍少，语料主要是人工整理内容，还需收集板端 ASR 误识别和真实用户纠偏。以上分数不能视为上线准确率保证。

模型置信度低于 `confidence_threshold` 时，意图转为普通对话；纠偏模型超过 `correction_threshold`，且与 60 秒内上一条分类不同，才保存待复核样本。样本位于 `/userdata/config/intent_node/intent_feedback.sqlite3`，不会自动改变模型。板端推理延迟、RSS、CPU 和实际麦克风端到端准确率尚未测量。

本地职责仍按能力分开：`music_node` 处理音乐播放，`audio_io_node` 负责麦克风、语音输出及共享音量，`memory_node` 管理记忆和相对提醒。分类器应将结果路由成这些节点接收的类型化命令，不合并成一个全能节点。

## 官方资料

- fastText 监督分类、C++ 构建和标注数据：[教程](https://fasttext.cc/docs/en/supervised-tutorial.html)。模型压缩和 CPU 推理：[FAQ](https://fasttext.cc/docs/en/faqs.html)。
- 中文 RoBERTa Tiny 的结构、预训练和任务微调说明：[模型卡](https://huggingface.co/uer/chinese_roberta_L-2_H-128)。12.8 MB 文件：[文件列表](https://huggingface.co/uer/chinese_roberta_L-2_H-128/tree/main)。
- BGE 参数量、用途和相似度限制：[模型卡](https://huggingface.co/BAAI/bge-small-zh-v1.5)。95.8 MB 文件：[文件列表](https://huggingface.co/BAAI/bge-small-zh-v1.5/tree/main)。
- Qwen 491 MB 量化文件：[官方 GGUF 文件列表](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/tree/main)。C/C++ 实现：[llama.cpp](https://github.com/ggml-org/llama.cpp)。JSON 格式约束：[语法说明](https://github.com/ggml-org/llama.cpp/blob/master/grammars/README.md)；格式约束不保证意图或参数正确。
- NPU 平台支持：[RKNN-Toolkit2](https://github.com/airockchip/rknn-toolkit2)、[RKLLM](https://github.com/airockchip/rknn-llm)。
- C++ 推理：[ONNX Runtime](https://onnxruntime.ai/docs/get-started/with-cpp.html)。量化与 Transformer 建议：[量化文档](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)。
- DeepSeek 工具声明、客户端执行和结果回传：[官方 Tool Calls 文档](https://api-docs.deepseek.com/guides/tool_calls/)。
