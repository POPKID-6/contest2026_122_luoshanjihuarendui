# 厨房防干烧预警系统 AI Coding 日志

GitHub 用户：POPKID-6；工具：Codex Desktop。

本目录包含 8 个主会话（含协作子线程），按北京时间的 9 个日期整理为 14 个 JSONL 文件，共 3574 条事件。manifest.json 为会话清单。内容来自本机真实历史会话，覆盖本项目方案、硬件与固件联调、算法、厨房语音 Skill、报告和源码提交。

## 历史记录转换说明

根据参赛者转达的组委会同意历史日志转换的意见，从原始 Codex Desktop 记录转换为官方 schema 1.0。保留原始会话 ID、UTC 时间戳、可见消息、工具调用和结果；日期目录采用北京时间。协作子线程归入主会话，并保留来源线程和原始行号等元数据。seq 为转换后的连续索引。

已排除无关项目、日志导出讨论、内部推理、系统提示、凭据和二进制载荷。脱敏、媒体省略、原始截断及缺失均保留说明；没有补写历史对话或工具结果。历史讨论不代表每项设想均已实现。

官方 schema 尚无 Desktop 历史转换枚举，collection_mode 使用 vscode_extension_partial 兼容值；同时明确记录 source_app=Codex Desktop、import_method=historical_transcript_conversion。health=degraded 表示历史数据省略和局限，不表示使用了 VS Code 插件或 CLI 自动采集，也不表示转换失败。

官方 validate-log.py 已通过，并已核对源记录、日期归档、连续序号和脱敏情况。原始私人会话快照不随仓库公开。

[官方日志归集与提交手册](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)
