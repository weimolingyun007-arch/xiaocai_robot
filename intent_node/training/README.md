# IntentNode 意图模型

`IntentNode` 使用本地训练的字符 n-gram softmax 模型决定意图类别。另一个模型头判断当前表达是否在纠正刚才的意图。运行时不使用正则表达式或关键词规则判定音乐、音量、记忆、提醒和闲聊类别。

分类结果映射到项目已有的类型化 ROS 消息。参数提取与输入校验仍由代码完成：例如从模型判为“设置音量”的句子中读取数值，从提醒句中读取时长和内容；缺少必填参数时转为澄清对话。类别映射不负责判断用户说的是什么意图。

## 模型与数据

- `data/intent_examples.jsonl`：普通意图样本，包含闲聊、命令否定和提问等负例。
- `data/intent_correction_examples.jsonl`：带目标意图标签的用户纠偏表达，训练意图类别和纠偏检测两个模型头。
- `intent_labels.json`：意图标签到 C++ ROS 动作的固定映射名。
- `../assets/intent_model.json`：训练生成的轻量模型，随运行包部署到 `resource/intent/intent_model.json`。

数据按 `train`、`validation`、`test` 分开。纠偏检测训练样本包含正例和带相似措辞的普通询问负例。当前数据主要是人工整理的示例，板端真实 ASR 样本还需要持续补充；目前测试集结果用于发现明显问题，不能代表真实环境准确率。

训练程序只依赖 Python 标准库，在开发机运行即可，不需要在板端安装 PyTorch、Transformers 或 ONNX Runtime。

## 训练、评估和预测

在工程根目录运行：

```bash
python3 intent_node/training/train_intent_classifier.py
python3 intent_node/training/evaluate_intent_classifier.py --split test
python3 intent_node/training/predict_intent.py --text "不是播放音乐，是暂停"
```

训练脚本固定随机种子，使用训练集拟合意图类别与纠偏检测模型，并分别输出 validation、test 上的准确率、Macro-F1 与纠偏 Precision/Recall/F1。完成训练后，再运行 `make build && make ota` 生成包含新模型的板端包。

## 板端行为

模型配置在 `intent_node/config/biz_config.toml`：

```toml
[classifier]
model_path = "intent/intent_model.json"
confidence_threshold = 0.25
correction_threshold = 0.65
```

意图置信度低于阈值时转给对话节点；纠偏模型超过阈值时，若最近 60 秒内存在不同类别的上一条意图，`IntentNode` 会执行新类别并把原话、纠偏原句、前后标签和原预测分数保存到 SQLite。纠偏样本初始状态为 `pending`，不会自动改训练集或在线更新模型。

数据库位于 `/userdata/config/intent_node/intent_feedback.sqlite3`。ROS 服务 `/xiaocai_robot/intent/feedback` 也可接收客户端提交的纠偏样本。人工复核后再将样本加入 JSONL，并分配数据集 split；不要把同一句或近似模板同时放进训练集和测试集。

早期 `hfl/rbt3` 实验的 checkpoint 和 ONNX 文件已清理。当前训练脚本使用字符 n-gram 模型，运行节点只加载 `intent_model.json`。
