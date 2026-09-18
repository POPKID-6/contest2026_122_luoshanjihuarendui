# openvela 智能厨房网关

本目录提供运行在联网电脑上的 Python 网关及自检工具。Gemini-S1 设备通过 WiFi/MQTT 上传文字或录音，网关调用语音识别、大模型和语音合成服务，并把回答回传设备；同时提供微信等消息推送和定时提醒。设备端防干烧检测独立于本目录，网关接收设备已生成的告警。

本次源码整理仅进行了 Python AST 语法解析及离线路径、配置一致性检查，未运行网关、自检脚本、网络服务或硬件测试。

## 目录

| 文件 | 用途 |
|---|---|
| `gateway.py` | MQTT 接入、ASR/LLM/TTS 调用、推送与定时排程 |
| `config.ini.sample` | 无凭据的配置样例，复制为 `config.ini` 后使用 |
| `requirements.txt` | Python 依赖 |
| `启动网关.bat` | Windows 启动脚本，进程退出后等待 5 秒重启 |
| `自检工具/` | 独立链路检查与音频重发脚本 |

## 安装与启动

在本目录打开 PowerShell，使用已安装的 Python 3：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
Copy-Item -LiteralPath config.ini.sample -Destination config.ini
```

仅在第一次配置时复制样例；已有 `config.ini` 时直接编辑现有文件，保留自己的设置。安装包为 `paho-mqtt>=1.6,<2` 和 `dashscope`。代码通过 Python 标准库 `urllib` 发起 HTTP 请求，不需要单独安装 `requests`。

编辑配置后启动：

```powershell
.\.venv\Scripts\python.exe gateway.py
```

`启动网关.bat` 调用的是当前环境中的 `python`。若使用上面的虚拟环境，可以在 PowerShell 中执行：

```powershell
$env:PATH = (Join-Path $PWD '.venv\Scripts') + ';' + $env:PATH
.\启动网关.bat
```

配置只在进程启动时读取，**修改后须重启网关**。同一设备通常只启动一个网关实例，避免一条请求产生多次回答或重复提醒。

## 配置说明

| 配置项 | 样例默认值 / 用法 |
|---|---|
| `[mqtt] host / port` | `broker.emqx.io / 1883`，公共演示 broker |
| `[mqtt] device_id` | `device001`；须与设备端配置一致 |
| `[mqtt] username / password` | 留空；网关支持配置账号，但是否可用还取决于设备端和 broker 的匹配配置 |
| `[push] channel` | 默认 `none`；可选 `serverchan`、`pushplus`、`wecom`、`dingtalk` |
| 推送 Key / token / webhook | 仅填写所选通道所需字段；样例全部留空 |
| `[ai] llm_base_url` | DashScope 的 OpenAI 兼容 API 地址 |
| `[ai] llm_api_key / llm_model` | Key 默认留空，模型默认 `qwen-plus` |
| `[ai] asr_api_key / asr_model` | Key 默认留空，模型默认 `paraformer-realtime-v2` |
| `[ai] tts_api_key / tts_model / tts_voice` | Key 默认留空，默认 `cosyvoice-v2 / longxiaochun_v2` |

设备与网关须使用相同的 broker、端口和 `device_id`。公共 broker 上的 `device001` 是示例主题；部署多个项目时应为设备选择独立 ID，并同步修改两端配置。当前交付设备端未实现 MQTT 账号鉴权，切换 broker 时需一并核对其能力。

LLM Key 留空时使用本地规则回复，ASR/TTS Key 留空时不会生成真实语音识别或语音合成结果。不要用非空占位字符串代替空 Key，否则会尝试调用服务。规则回复仍通过 MQTT 收发，因此默认公共 broker 需要联网；它不等同于整个系统可脱网运行。原样例中的 `mock_when_no_key` 并未被代码读取，现已移除该无效配置项。

真实凭据只写入本机 `config.ini`，不写入源码和公开 README。

## 功能与协议

| 功能 | 当前实现 |
|---|---|
| 文字问答 | 设备发送 JSON 文字请求；网关调用大模型或返回规则答案 |
| 语音问答 | 上传 16 kHz、16 bit、单声道裸 PCM，网关 ASR 后进入问答 |
| 语音合成 | 将回答合成为 16 kHz 单声道 WAV，发布至设备音频主题 |
| 定时提醒 | 解析秒、分钟、小时，使用网关 `threading.Timer` 排程，到时推送并下发 `alarm` |
| 微信转发 | `type=wechat` 将设备提供的文字转发到已配置的推送通道 |
| 告警推送 | 接收告警 JSON，生成处置建议并推送；不能据此推断设备测温精度 |
| 状态消息 | 接收设备 `state` 主题，在控制台输出 |

定时器保存在网关进程内存中，网关退出或重启后不会恢复已有任务。设备正常播放 TTS 还依赖其固件、音频驱动和输出硬件；收到 WAV 不等于播放已验证。

`<dev>` 为 `device_id`：

| 方向 | MQTT 主题 | 载荷 |
|---|---|---|
| 设备→网关 | `openvela/kitchen/<dev>/ai/request` | `{"type":"text","text":"炖鱼要多久？"}`、`{"type":"wechat","text":"待转发内容"}`，或**裸 PCM 二进制** |
| 网关→设备 | `openvela/kitchen/<dev>/ai/response/text` | **纯 UTF-8 文本**，不是 JSON 包装 |
| 网关→设备 | `openvela/kitchen/<dev>/ai/response/audio` | 16 kHz 单声道 WAV 字节流 |
| 网关→设备 | `openvela/kitchen/<dev>/cmd` | 定时设置 JSON（`type=timer`、`duration_sec`、`duration_min`、`event`）或到时提示（`type=alarm`、`event`） |
| 设备→网关 | `openvela/kitchen/<dev>/state` | 设备状态 JSON |
| 设备→网关 | `openvela/kitchen/<dev>/alert` | `{"temp":95.0,"hum":60,"risk":3,"reason":"manual_test"}`，此处仅为协议示例 |

## 自检工具

这些工具会连接外部服务；推送工具会发送消息，音频工具会向设备发布音频。表格说明实际脚本行为，本次整理没有运行这些操作。

| 工具 / 命令 | 实际用途和配置来源 |
|---|---|
| `python gateway.py testpush` | 读取本目录 `config.ini`，向所选推送通道发送一条测试消息；不需要连接 MQTT |
| `python 自检工具/check_keys.py` | 读取上一级 `config.ini`，直接请求大模型及 Server酱；会发送自检微信，与 `[push] channel` 选择无关，仅适合已配置 Server酱时使用 |
| `python 自检工具/mqtt_probe.py` | 模拟设备发送一条文字问答请求并接收下行；默认 broker 和设备 ID 写在脚本中，不读取 `config.ini` |
| `python 自检工具/mqtt_sniff.py` | 连接公共 broker，订阅 `openvela/kitchen/#`，打印收到的消息；原脚本日志路径保持不变，路径不可写时仍打印到控制台 |
| `python 自检工具/send_tts_test.py "测试语音内容"` | 导入上一级 `gateway.py` 并读取其配置，调用真实 TTS，再发布到配置设备的音频主题；生成文件保存为 `自检工具/test_tts.wav` |
| `python 自检工具/resend_tts.py` | 读取同目录 `test_tts.wav`，向脚本中固定的公共 broker / `device001` 音频主题重发 3 次，发送之间等待 40 秒 |

上述 `python` 可替换为 `.\.venv\Scripts\python.exe`。`check_keys.py`、`send_tts_test.py` 使用文件所在目录推导配置或模块位置，不依赖启动时工作目录。`mqtt_probe.py`、`mqtt_sniff.py`、`resend_tts.py` 保留原脚本中的固定 broker/主题；若修改主配置，需要同步检查这些脚本常量。原始套接字自检脚本未实现 MQTT 登录鉴权。

目录中没有 `sim_device.py` 或 `sim_wechat.py`，不提供这些旧文档命令。

## 现有实现限制

- 当前演示 MQTT 链路使用公共 broker 的明文 TCP 1883 端口；设备与网关的登录和传输能力需共同适配。
- `gateway.py` 保留历史 HTTP 证书校验失败后的宽松重试逻辑；正式部署前应配置可信证书并调整该逻辑。
- 网关请求超时控制和 MQTT 线程分离已体现在源码中，本次未进行并发、长时间运行或真机验证。
- 本目录仅整理源码、依赖和使用文档，不新增或重写历史测试日志。
