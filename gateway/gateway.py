#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
openvela 智能厨房 —— 云端网关 v2
================================
- 启动时读取同目录 config.ini（broker / 推送通道 / AI Key，修改后须重启）
- 设备 <-> MQTT <-> 网关：ai/request、ai/response/text、ai/response/audio、cmd、state、alert
- 可插拔推送：serverchan / pushplus / wecom / dingtalk / none
- 大模型：OpenAI 兼容接口（阿里云百炼 DashScope 兼容模式 / DeepSeek 均可）
- ASR/TTS：接口已预留（DashScope），未配置 Key 时走 mock（文本交互）

依赖：pip install -r requirements.txt
用法：python gateway.py            # 启动网关
      python gateway.py testpush   # 只测推送通道
"""
import configparser
import json
import os
import queue
import re
import ssl
import sys
import threading
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(HERE, "config.ini")

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None  # 仅在真正启动 MQTT 网关时才需要（testpush 不需要）


def load_cfg():
    c = configparser.ConfigParser()
    c.read(CFG_PATH, encoding="utf-8")
    return c


CFG = load_cfg()
MQTT_HOST = CFG.get("mqtt", "host", fallback="broker.emqx.io")
MQTT_PORT = CFG.getint("mqtt", "port", fallback=1883)
MQTT_USER = CFG.get("mqtt", "username", fallback="")
MQTT_PASS = CFG.get("mqtt", "password", fallback="")
DEV = CFG.get("mqtt", "device_id", fallback="device001")

PUSH_CHANNEL = CFG.get("push", "channel", fallback="none").strip().lower()
SERVERCHAN_KEY = CFG.get("push", "serverchan_key", fallback="").strip()
PUSHPLUS_TOKEN = CFG.get("push", "pushplus_token", fallback="").strip()
WECOM_WEBHOOK = CFG.get("push", "wecom_webhook", fallback="").strip()
DINGTALK_WEBHOOK = CFG.get("push", "dingtalk_webhook", fallback="").strip()

LLM_BASE = CFG.get("ai", "llm_base_url", fallback="").strip().rstrip("/")
LLM_KEY = CFG.get("ai", "llm_api_key", fallback="").strip()
LLM_MODEL = CFG.get("ai", "llm_model", fallback="qwen-plus").strip()
ASR_KEY = CFG.get("ai", "asr_api_key", fallback="").strip()
TTS_KEY = CFG.get("ai", "tts_api_key", fallback="").strip()
ASR_MODEL = CFG.get("ai", "asr_model", fallback="paraformer-realtime-v2").strip()
TTS_MODEL = CFG.get("ai", "tts_model", fallback="cosyvoice-v2").strip()
TTS_VOICE = CFG.get("ai", "tts_voice", fallback="longxiaochun_v2").strip()

TP = "openvela/kitchen/" + DEV
TOPIC_REQ = TP + "/ai/request"
TOPIC_RES_T = TP + "/ai/response/text"
TOPIC_RES_A = TP + "/ai/response/audio"
TOPIC_CMD = TP + "/cmd"
TOPIC_STATE = TP + "/state"
TOPIC_ALERT = TP + "/alert"

CTX = ssl.create_default_context()
if os.environ.get("GW_INSECURE") == "1":
    CTX = ssl._create_unverified_context()


# ----------------------------- 推送通道 -----------------------------
def http_post(url, data=None, json_body=None, headers=None, timeout=15):
    h = {"User-Agent": "openvela-kitchen-gw/2.0"}
    if headers:
        h.update(headers)
    if json_body is not None:
        body = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
        h["Content-Type"] = "application/json"
    else:
        body = urllib.parse.urlencode(data or {}).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers=h)

    def _do(ctx):
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.status, r.read().decode("utf-8", "replace")

    try:
        return _do(CTX)
    except Exception as e:
        msg = str(e)
        # 本机 CA/证书过期（常见于 anaconda 自带的证书链）会让推送直接失败，
        # 这里自动降级为不校验重试一次，避免"微信永远收不到"。
        if ("CERTIFICATE_VERIFY_FAILED" in msg or "certificate verify failed" in msg
                or "certificate has expired" in msg):
            print("[warn] SSL 证书校验失败，降级为不校验重试:", msg[:120])
            return _do(ssl._create_unverified_context())
        raise


def push(title, content):
    """按配置通道推送到手机（Server酱等）。返回 (ok, detail)"""
    try:
        if PUSH_CHANNEL == "serverchan":
            if not SERVERCHAN_KEY:
                return False, "未配置 serverchan_key"
            st, body = http_post("https://sctapi.ftqq.com/%s.send" % SERVERCHAN_KEY,
                                 data={"title": title, "desp": content})
            return st == 200, body[:200]
        if PUSH_CHANNEL == "pushplus":
            if not PUSHPLUS_TOKEN:
                return False, "未配置 pushplus_token"
            st, body = http_post("http://www.pushplus.plus/send",
                                 json_body={"token": PUSHPLUS_TOKEN,
                                            "title": title, "content": content})
            return st == 200, body[:200]
        if PUSH_CHANNEL == "wecom":
            if not WECOM_WEBHOOK:
                return False, "未配置 wecom_webhook"
            st, body = http_post(WECOM_WEBHOOK,
                                 json_body={"msgtype": "text",
                                            "text": {"content": title + "\n" + content}})
            return st == 200, body[:200]
        if PUSH_CHANNEL == "dingtalk":
            if not DINGTALK_WEBHOOK:
                return False, "未配置 dingtalk_webhook"
            st, body = http_post(DINGTALK_WEBHOOK,
                                 json_body={"msgtype": "text",
                                            "text": {"content": title + "\n" + content}})
            return st == 200, body[:200]
        return False, "通道=%s，未推送" % PUSH_CHANNEL
    except Exception as e:
        return False, "%s: %s" % (type(e).__name__, e)


# ----------------------------- 大模型 -----------------------------
def llm(prompt):
    """OpenAI 兼容对话；未配置 Key 时使用 mock 规则回答"""
    if not (LLM_BASE and LLM_KEY):
        return mock_llm(prompt)
    try:
        st, body = http_post(
            LLM_BASE + "/chat/completions",
            json_body={"model": LLM_MODEL,
                       "messages": [
                           {"role": "system",
                            "content": "你是专业厨房烹饪助手，回答简短口语化，"
                                       "控制在50字以内，适合语音播报。"},
                           {"role": "user", "content": prompt}],
                       "temperature": 0.7, "max_tokens": 200},
            headers={"Authorization": "Bearer " + LLM_KEY}, timeout=30)
        if st != 200:
            return "（大模型调用失败：HTTP %s）" % st
        data = json.loads(body)
        return data["choices"][0]["message"]["content"].strip()
    except Exception as e:
        return "（大模型异常：%s）" % e


def mock_llm(prompt):
    if any(k in prompt for k in ("炖", "煮", "蒸", "炒", "烤")):
        return "建议大火烧开后转中小火，约20-30分钟，期间注意水量别烧干。"
    if any(k in prompt for k in ("多久", "时间", "几分钟")):
        return "一般中火20-30分钟即可，中途看一眼水量更稳妥。"
    if any(k in prompt for k in ("干烧", "冒烟", "危险")):
        return "立即关火或调小火，并加入约200ml清水降温，注意安全。"
    return "已收到，我会帮你留意锅中状态，请注意用火安全。"


# ----------------------------- ASR / TTS（DashScope SDK） -----------------------------
def asr_bytes(pcm_bytes, sample_rate=16000):
    """设备上传的 16kHz 单声道 PCM -> 文本；未配置 Key 返回 None（走文本模式）"""
    if not ASR_KEY:
        return None
    try:
        os.environ["DASHSCOPE_API_KEY"] = ASR_KEY
        from dashscope.audio.asr import Recognition, RecognitionCallback
    except ImportError:
        print("[ASR] 缺少 dashscope：pip install dashscope")
        return None

    final = {"text": ""}

    class _Cb(RecognitionCallback):
        def on_event(self, result):
            try:
                s = result.get_sentence()
                if isinstance(s, dict) and s.get("text"):
                    final["text"] = s["text"]      # 取最后（完整）一句
            except Exception:
                pass

        def on_error(self, result):
            print("[ASR] error:", result)

    try:
        rec = Recognition(model=ASR_MODEL, format="pcm",
                          sample_rate=sample_rate, callback=_Cb())
        rec.start()
        step = 3200
        for i in range(0, len(pcm_bytes), step):
            rec.send_audio_frame(pcm_bytes[i:i + step])
        rec.stop()
        time.sleep(0.5)
    except Exception as e:
        print("[ASR] exception:", type(e).__name__, e)
        return None
    return final["text"] or None


TIMERS = []          # [(fire_at_ts, event, human)]


def schedule_timer(sec, evt, human):
    """真正到点提醒：到时间后推送手机 + 下发设备提示"""
    fire_at = time.time() + sec
    TIMERS.append((fire_at, evt, human))
    print("[定时] 已排程 %s 后提醒：%s（当前共 %d 个）" % (human, evt, len(TIMERS)))

    def fire():
        TIMERS[:] = [t for t in TIMERS if t[0] != fire_at]
        msg = "提醒时间到啦：%s" % evt
        ok, detail = push("[厨房助手] 定时提醒", msg)
        print("[定时触发] %s | push=%s" % (msg, ok))
        try:
            # 先发指令（设备端据此响铃），再发友好文案，
            # 这样设备屏幕上最后显示的是中文提示而不是原始 JSON
            client.publish(TOPIC_CMD,
                           json.dumps({"type": "alarm", "event": evt},
                                      ensure_ascii=False), qos=1)
            time.sleep(0.2)
            downlink_text(msg)
        except Exception as e:
            print("[定时触发] 下发失败:", e)

    t = threading.Timer(sec, fire)
    t.daemon = True
    t.start()
    return t


def tts_bytes(text):
    """文本 -> 16kHz WAV 字节；未配置 Key 返回 None（只显示文本）"""
    if not TTS_KEY:
        return None
    try:
        os.environ["DASHSCOPE_API_KEY"] = TTS_KEY
        from dashscope.audio.tts_v2 import SpeechSynthesizer, AudioFormat
    except ImportError:
        print("[TTS] 缺少 dashscope：pip install dashscope")
        return None
    try:
        synth = SpeechSynthesizer(model=TTS_MODEL, voice=TTS_VOICE,
                                  format=AudioFormat.WAV_16000HZ_MONO_16BIT)
        return synth.call(text)
    except Exception as e:
        print("[TTS] exception:", type(e).__name__, e)
        return None


# ----------------------------- 业务逻辑 -----------------------------
client = None
if mqtt is not None:
    # client_id 必须唯一：若与另一个网关进程重名，broker 会让两者互相踢下线
    # （表现为日志里刷屏的"MQTT 已连接"，期间收不到任何设备请求）
    client = mqtt.Client(client_id="gw_kitchen_%d" % os.getpid())
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)


def call_with_timeout(fn, timeout, *a):
    """在独立线程里执行并限时等待。
    ASR/TTS 用的 DashScope SDK 是阻塞调用，一旦卡住会把整个网关拖死
    （旧版在 MQTT 回调里串行处理，一个请求卡住后面全部不响应）。"""
    box = {}

    def run():
        try:
            box["v"] = fn(*a)
        except Exception as e:
            box["e"] = e

    t = threading.Thread(target=run, daemon=True)
    t0 = time.time()
    t.start()
    t.join(timeout)
    if t.is_alive():
        print("[超时] %s 超过 %ds，放弃本次（后续请求不受影响）"
              % (getattr(fn, "__name__", fn), timeout))
        return None
    if "e" in box:
        print("[异常] %s: %s" % (getattr(fn, "__name__", fn), box["e"]))
        return None
    print("[耗时] %s %.1fs" % (getattr(fn, "__name__", fn), time.time() - t0))
    return box.get("v")


def save_audio_debug(payload):
    """把设备上传的语音存下来并算音量：
    RMS≈0 说明麦克风没采到声音（采集链路问题），
    RMS 正常但识别不出才轮到 ASR 的问题。"""
    try:
        path = os.path.join(HERE, "last_audio.pcm")
        with open(path, "wb") as f:
            f.write(payload)

        n = len(payload) // 2
        if n > 0:
            import array
            a = array.array("h")
            a.frombytes(payload[:n * 2])
            peak = max(max(a), -min(a))
            rms = (sum(float(v) * v for v in a) / n) ** 0.5
        else:
            peak = rms = 0

        print("[音频] 已存 %s  时长=%.1fs(%d 字节)  峰值=%d  RMS=%.1f"
              % (path, len(payload) / 32000.0, len(payload), peak, rms))
        if peak < 50:
            print("[音频] ★ 采集到的基本是静音：检查麦克风/录音设备（不是 ASR 的问题）")
    except Exception as e:
        print("[音频] 保存失败:", e)


def downlink_text(text):
    """下行文本：直接发纯 UTF-8 文本。
    设备端会把它原样显示、并作为“To WeChat”转发内容，
    所以这里不能发 JSON（否则屏幕上会出现 {"text":...}）。"""
    client.publish(TOPIC_RES_T, text.encode("utf-8"), qos=1)
    print("[下行文本]", text)


def handle_request(payload):
    text = None
    req_type = "text"
    obj = None

    try:
        obj = json.loads(payload)
    except Exception:
        obj = None

    if isinstance(obj, dict):
        text = obj.get("text", "")
        req_type = obj.get("type", "text")
    elif obj is not None:
        text = str(obj)          # 合法但非对象的 JSON：当文本处理，别崩
    else:
        # 非 JSON 载荷 = 设备上传的 16k 单声道 PCM
        print("[ASR] 收到语音 %d 字节，开始识别 ..." % len(payload))
        # 先给设备一个即时回执：否则识别期间设备会一直显示"等待"，久一点就报超时
        downlink_text("收到语音，正在识别…")
        save_audio_debug(payload)
        text = call_with_timeout(asr_bytes, 25, payload)
        if not text:
            downlink_text("语音识别超时或失败，请重试（也可以直接打字提问）")
            return

        print("[ASR] 识别结果:", text)

    if not text:
        return

    # 设备端“To WeChat”按钮：把这段文字推送到用户微信
    if req_type == "wechat":
        ok, detail = push("[厨房助手] 设备消息", text)
        print("[微信转发] ok=%s detail=%s" % (ok, detail))
        downlink_text("已发送到微信" if ok else "微信发送失败，请检查网关配置")
        return

    # 定时指令
    m = re.search(r"(\d+)\s*(分钟|分|秒|小时|个小时)", text)
    if any(k in text for k in ("提醒", "定时")) and m:
        num, unit = int(m.group(1)), m.group(2)
        if unit in ("分钟", "分"):
            sec = num * 60
        elif unit == "秒":
            sec = num
        else:
            sec = num * 3600
        sec = max(1, sec)
        minutes = max(1, sec // 60)
        evt = re.sub(r".*(提醒|定时)", "", text).strip("我你，。 ") or text[:20]
        cmd = {"type": "timer", "duration_sec": sec, "duration_min": minutes,
               "event": evt}
        client.publish(TOPIC_CMD, json.dumps(cmd, ensure_ascii=False), qos=1)
        human = ("%d 秒" % sec) if sec < 60 else (
            "%d 分钟" % (sec // 60) if sec % 60 == 0 else
            "%d 分 %d 秒" % (sec // 60, sec % 60))
        ans = "好的，已设置%s提醒：%s" % (human, evt)
        downlink_text(ans)
        schedule_timer(sec, evt, human)
        return

    ans = call_with_timeout(llm, 35, text) or mock_llm(text)
    downlink_text(ans)

    audio = call_with_timeout(tts_bytes, 30, ans)
    if audio:
        # QoS1：大包丢了能重传（设备侧已能正确回 PUBACK）
        client.publish(TOPIC_RES_A, audio, qos=1)
        print("[下行语音] %d bytes WAV" % len(audio))
    else:
        print("[下行语音] 本次无音频（TTS 超时/失败/未配置）")


def handle_alert(payload):
    try:
        d = json.loads(payload)
    except Exception:
        d = {"raw": payload.decode("utf-8", "replace")}
    risk = d.get("risk", 0)
    temp = d.get("temp", "?")
    hum = d.get("hum", "?")
    reason = d.get("reason", "未知")

    prompt = ("厨房锅具温度%s℃，湿度%s，风险等级%s，原因%s。"
              "用简短口语给出处置建议，30字以内。" % (temp, hum, risk, reason))
    advice = call_with_timeout(llm, 30, prompt) or "（AI 建议获取失败，请先关火检查）"

    title = "[厨房干烧预警] 风险等级 %s" % risk
    body = "温度: %s℃\n湿度: %s\n原因: %s\n\nAI 建议：%s" % (temp, hum, reason, advice)
    ok, detail = push(title, body)
    print("[告警推送] ok=%s detail=%s" % (ok, detail))

    try:
        if int(risk) >= 2:
            downlink_text("检测到高风险，已推送微信，请立即检查灶台！")
    except Exception:
        pass


def worker(topic, payload):
    """每个请求独立线程处理：
    之前用单线程队列，语音请求要跑 5~8 秒(ASR+LLM+TTS)，
    连点几次后面的就会排队超过设备 20 秒的等待上限 -> 屏幕显示"云端无响应"。"""
    try:
        if topic == TOPIC_REQ:
            handle_request(payload)
        elif topic == TOPIC_ALERT:
            handle_alert(payload)
    except Exception as e:
        print("[worker] 处理异常:", type(e).__name__, e)


def on_connect(c, u, flags, rc):
    print("MQTT 已连接:", MQTT_HOST)
    for t in (TOPIC_REQ, TOPIC_ALERT, TOPIC_STATE):
        c.subscribe(t, qos=1)
        print("  订阅:", t)


def on_message(c, u, msg):
    print("[收到]", msg.topic, "len=%d" % len(msg.payload))
    if msg.topic in (TOPIC_REQ, TOPIC_ALERT):
        threading.Thread(target=worker, args=(msg.topic, msg.payload),
                         daemon=True).start()


if __name__ == "__main__":
    print("=" * 60)
    print("openvela 智能厨房网关 v2")
    print("broker   :", MQTT_HOST, MQTT_PORT)
    print("device   :", DEV)
    print("push     :", PUSH_CHANNEL)
    print("llm      :", LLM_MODEL if (LLM_BASE and LLM_KEY) else "mock（未配置 Key）")
    print("=" * 60)
    if len(sys.argv) > 1 and sys.argv[1] == "testpush":
        ok, detail = push("厨房助手通道测试",
                          "若你在微信收到此消息，说明推送通道已打通。")
        print("push ok=%s detail=%s" % (ok, detail))
        sys.exit(0 if ok else 2)
    if mqtt is None:
        print("缺少依赖：pip install -r requirements.txt（testpush 不需要 MQTT 依赖）")
        sys.exit(1)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, 60)

    print("已就绪：每个请求独立线程处理（不排队、不阻塞 MQTT 循环）")
    client.loop_forever()
