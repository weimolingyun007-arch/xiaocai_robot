# intent_node 意图分类器训练

本目录提供 `hfl/rbt3` 中文意图分类器的微调、评估、ONNX 导出和动态 INT8 量化脚本。模型在开发电脑上训练；训练得到的分类结果映射回 `IntentNode` 现有的 `ChatIntent`、`MusicIntent`、`VolumeIntent`、`MemoryIntent` 和 `DialogueIntent`。模型只判断动作类别，参数解析和执行校验仍由项目代码负责。

## 当前范围

- 标签名称及到项目动作的映射由 `intent_labels.json` 固定，标签名对应当前 `IntentResultName()` 使用的业务名称。
- `intent_examples.jsonl` 是人工编写的第一版种子语料，每类 20 条，分为 14 条训练、3 条验证、3 条测试。`source: "seed"` 表示这些不是板端真实 ASR 转写。
- 这份小语料只用来跑通训练流程和发现类别设计问题。3 条/类的测试集不足以证明泛化能力；部署前应扩充到每类数十条以上，并加入实际 ASR 转写和人工复核的误识别样本。
- 当前机器人支持延迟提醒，没有独立的绝对时间闹钟执行能力。提到“明早七点设闹钟”等句子先映射到 `reminder_clarify`；不要把它当成闹钟已实现。
- 当前 ROS 节点仍使用原有 `IntentRules`。本目录不会在没有训练模型、C++ tokenizer 和运行时验证前改变节点行为。

## 安装训练环境

在开发电脑创建隔离环境，避免在 RK3566 板端安装训练用的 PyTorch：

```bash
cd intent_node/training
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

`hfl/rbt3` 的预训练权重由 Transformers 首次运行时下载。模型是中文三层 RoBERTa 预训练底座，需要用本项目的标注语句微调后才能分类。[模型卡](https://huggingface.co/hfl/rbt3)

## 微调

```bash
python train_intent_classifier.py \
  --data data/intent_examples.jsonl \
  --labels intent_labels.json \
  --model-name hfl/rbt3 \
  --output-dir output \
  --epochs 8
```

脚本会检查标签是否齐全、每个 split 是否覆盖全部类别，以及完全相同的语句是否跨 split 泄漏；训练集和验证集的 Macro-F1 用于选择最佳 checkpoint。训练参数可在命令行调整。实现流程使用 Transformers 的序列分类头和 `Trainer` 微调。[官方序列分类指南](https://huggingface.co/docs/transformers/main/tasks/sequence_classification)

## 评估

```bash
python evaluate_intent_classifier.py --model-dir output --split validation
python evaluate_intent_classifier.py \
  --model-dir output \
  --split test \
  --output output/test_metrics.json
python predict_intent.py --model-dir output --text "放音乐"
```

报告包括每类 Precision、Recall、F1、混淆矩阵、每条语句的预测置信度和前两名分差。先检查混淆矩阵和错误样本，再补充或修正语料。实际运行时的低置信度阈值应根据更大的验证集选择，不能直接把 softmax 分数当成准确率。

## 导出 ONNX 与动态 INT8

```bash
bash export_onnx.sh output onnx
python quantize_onnx.py onnx/model.onnx onnx/model_int8.onnx
```

Optimum 的导出器支持 `text-classification` 任务；动态量化后还需要对照 FP32 的准确率、延迟和峰值内存。[ONNX 导出指南](https://huggingface.co/docs/optimum-onnx/onnx/usage_guides/export_a_model)

导出文件和 checkpoint 位于本目录下的 `output/`、`onnx/`，已加入本地忽略规则，不会随代码提交。当前仓库只有 ONNX Runtime 共享库，没有配套 C++ API 头文件和 tokenizer 实现，因此 ONNX 文件生成后还不能直接替换运行中的规则分类器。接入前需补齐 tokenizer/推理实现，在主机及板端验证，并保持低置信度、复合请求和参数校验路径。

## 扩充数据

添加样本时保留 JSONL 格式：

```json
{"text":"给我放点舒服的背景音乐","label":"music_play","split":"train","source":"asr"}
```

标签必须在 `intent_labels.json` 中。将 `split` 标为 `train`、`validation` 或 `test`；同一意图的同一模板及其近似改写不要分散到多个 split。真实 ASR 文本经人工确认标签后标记 `source: "asr"`。否定、询问、复合请求、缺参及“只是提到某个命令词”的句子要有覆盖，避免模型把讨论命令误判成执行命令。
