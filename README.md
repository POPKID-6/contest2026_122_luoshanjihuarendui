# 小K厨房助手

**2026 首届 openvela AI 硬件开发者大赛 · AI 硬件产品创新赛道**

**队伍：洛杉矶湖人队（122）**

小K面向家庭烹饪场景，将锅具温度监测、干烧风险分析、厨房语音问答和远程提醒结合起来。项目使用主办方提供的 Gemini-S1 运行 openvela，使用 ESP32-S3 与 GY-906 BCC / MLX90614ESF 非接触式红外测温模块采集温度，两个设备通过 WiFi 通信。

本仓库集中管理设备端应用、Python 网关、锅具识别与干烧算法，以及构建和配置说明。

## 功能与技术方案

| 模块 | 实现内容 |
|---|---|
| Gemini-S1 应用 | LVGL 触摸界面、WiFi 扫描与配网、文字提问、录音、回答显示、音频播放及告警交互。 |
| 消息通信 | MQTT 3.1.1 消息收发、断线重连、心跳维护和音频数据接收。 |
| 语音与问答网关 | 接收设备请求，连接 Qwen、Paraformer ASR 和 CosyVoice TTS，返回文本及语音。 |
| 提醒与通知 | 解析定时提醒请求，发送到期提示；处理设备告警及微信推送。 |
| 温度算法 | 温度曲线平滑、局部拟合、平台/过冲特征、锅型规则识别、持续确认及报警锁存。 |
| 可选模型路径 | 七特征 XGBoost 分类与阈值建议预测，提供独立训练入口。 |

硬件测温链路为 `GY-906 → ESP32-S3 → WiFi → Gemini-S1`；设备与 Python 网关之间使用 MQTT。Python 算法以时间戳和温度为输入，返回锅型、过程状态与风险结果。openvela 侧使用 LVGL 图形交互、网络/WiFi 接口和录音播放能力；网关部署在可联网的电脑或服务器上。

## 目录说明

```text
app/kitchen_smart/                 Gemini-S1 C 应用与 Kconfig/Makefile
gateway/                          Python 网关、配置样例和辅助工具
algorithms/pot_algorithms/         温度特征、锅型识别、干烧检测和训练接口
algorithms/README.md               算法 API、输入单位、示例及测试说明
docs/gemini_build.md               工程接入、配置、编译与烧录
docs/board/                        板级配置说明
docs/audio/                        音频驱动修改说明
contest2026_122_luoshanjihuarendui.xml  本队工程清单与应用目录映射
openvela.xml                      openvela 基础工程清单
logs/                             赛事 AI Coding 日志目录
```

`app/hello_app/`、`board/contest_board/`、`quickapp/hello_quickapp/` 是赛事仓库原有示例；小K应用入口为 `app/kitchen_smart/`。

本次源码来自厨房助手交付包与 `pot_algorithms` 算法包。设备应用保留交付版的演示温湿度刷新入口，算法按传入样本独立计算；这两个包未包含 ESP32-S3 采集固件、最终传感器接收适配工程及外部模型/数据文件。板级和音频目录提供配置项与修改片段，具体工程接入见构建说明。

## 获取源码

仅运行算法或网关时，克隆本仓库即可：

```bash
git clone -b dev-ai-contest-2026 https://github.com/open-vela/contest2026_122_luoshanjihuarendui.git
cd contest2026_122_luoshanjihuarendui
```

构建 Gemini-S1 固件时，在已配置 openvela 开发环境的 Linux 工作目录中拉取完整工程：

```bash
repo init -u https://github.com/open-vela/contest2026_122_luoshanjihuarendui \
  -b dev-ai-contest-2026 -m contest2026_122_luoshanjihuarendui.xml
repo sync -c -j8
```

清单将 `app/kitchen_smart/` 映射为 `packages/demos/contest2026_122_kitchen_smart`，其 `Make.defs` 使用同一目录注册应用。构建在 openvela 工作区根目录执行，步骤见 [Gemini-S1 构建说明](docs/gemini_build.md)。

## 快速运行算法

需要 Python 3.10 或更高版本。规则算法和现有测试只依赖 Python 标准库。

```bash
cd algorithms
python -X utf8 -m unittest pot_algorithms.test_algorithms -v
python -X utf8 -c "from pot_algorithms.example_usage import classify_by_curve; print(classify_by_curve(0.940, 0.8, -0.02))"
```

时序检测由调用方持续提供样本，并复用同一个检测器：

```python
from pot_algorithms import DryBurnDetector, PotCategory

detector = DryBurnDetector(category=PotCategory.UNKNOWN)
result = detector.update(timestamp_s=0.0, temperature_c=30.0)
print(result.state, result.alarm_active)
```

时间单位为秒，温度单位为摄氏度；时间戳需严格递增。无数据时由调用方调用 `check_stale()` 检查超时。训练与模型推理是可选路径，依赖安装、数据列定义和 API 见 [算法说明](algorithms/README.md)。

## 启动语音网关

在仓库根目录创建 Python 虚拟环境并安装依赖：

```bash
python -m venv .venv
# Windows PowerShell
.venv\Scripts\Activate.ps1
# Linux/macOS 使用：source .venv/bin/activate
python -m pip install -r gateway/requirements.txt
python -c "from pathlib import Path; p=Path('gateway/config.ini'); p.exists() or p.write_bytes(Path('gateway/config.ini.sample').read_bytes())"
```

编辑本地 `gateway/config.ini`，设置 MQTT 地址、设备 ID、云端模型密钥和推送通道。设备与网关的 broker、端口及 `device_id` 必须一致。完成后启动：

```bash
python gateway/gateway.py
```

样例使用公共 MQTT broker 与 `device001`；多人演示时使用双方一致的独立设备 ID。无云端密钥时将相应密钥项留空，可以使用规则文本回复；ASR/TTS 需要有效服务配置。修改配置后重新启动网关。

配置文件只保留在本地，仓库提供 `config.ini.sample`。消息格式和辅助脚本见 [网关说明](gateway/README_zh.md)。

## Gemini-S1 使用流程

1. 按 [构建说明](docs/gemini_build.md) 配置固件、编译打包并烧录。
2. 在 WiFi 页面扫描热点、输入密码并连接网络。
3. 启动与设备使用相同 MQTT 配置的网关。
4. 在 AI 页面发送文本，或通过 `Rec 8s` 录音发起厨房问答。
5. 接收文本与音频响应；可通过 `To WeChat` 转发回答。
6. 使用告警页测试告警消息，或输入“提醒我1分钟后关火”检查提醒流程。

推送和提醒操作会触发配置的通知服务。自动温度判定接入应使用实际采集样本，并明确算法返回状态与设备风险显示的映射。

## 本次源码验证

- 算法现有 28 项 `unittest` 全部通过。
- 规则示例返回 `ALCU`；90 点恒温模拟输入返回 `BOILING`，未激活报警。
- Python 文件通过语法检查，工程清单的应用映射与 `Make.defs` 路径已对齐。
- 上述结果来自本地代码检查与模拟数据；本次整理未执行开发板交叉编译、烧录或联网服务测试。

## AI 辅助开发与赛事资料

项目使用 Codex 辅助开发。AI Coding 历史日志已归档至 [logs/POPKID-6/](logs/POPKID-6/)，包含 14 个 JSONL 文件、3574 条事件，并已通过官方格式校验。日志来自真实 Codex Desktop 会话，转换方式、脱敏范围与历史数据局限见 [日志说明](logs/POPKID-6/README.md)。

- [参赛代码提交指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/code_submission_guide.md)
- [AI Coding 日志归集与提交手册](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)
- [AI 硬件赛道教程](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_hardware_guide_index.md)

## 开源协议

本项目源码按 [Apache License 2.0](LICENSE) 发布。openvela 及其他依赖遵循各自仓库的许可证。
