# -*- coding: utf-8 -*-
"""直接验证 config.ini 里的 Key 是否可用（不经过 MQTT）"""
import configparser, json, os, ssl, sys, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY_DIR = os.path.dirname(HERE)
cfg = configparser.ConfigParser()
cfg.read(os.path.join(GATEWAY_DIR, "config.ini"), encoding="utf-8")

llm_key = cfg.get("ai", "llm_api_key", fallback="").strip()
base = cfg.get("ai", "llm_base_url", fallback="").strip().rstrip("/")
model = cfg.get("ai", "llm_model", fallback="qwen-plus").strip()
sc_key = cfg.get("push", "serverchan_key", fallback="").strip()

print("=== 1) 大模型 (DashScope OpenAI 兼容) ===")
print("url  :", base + "/chat/completions")
print("model:", model, " key:", llm_key[:8] + "..." + llm_key[-4:])
try:
    req = urllib.request.Request(
        base + "/chat/completions",
        data=json.dumps({"model": model,
                         "messages": [{"role": "user", "content": "只回两个字：你好"}]},
                        ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + llm_key})
    with urllib.request.urlopen(req, timeout=40) as r:
        body = r.read().decode("utf-8", "replace")
        print("HTTP", r.status, "->", body[:400])
    print("结论: 大模型 Key 可用")
except urllib.error.HTTPError as e:
    print("HTTP 错误", e.code, e.read().decode("utf-8", "replace")[:400])
    print("结论: ★ 大模型 Key 不可用（这就是不回话的直接原因）")
except Exception as e:
    print("异常:", type(e).__name__, e)
    print("结论: ★ 无法访问 DashScope（网络/代理问题）")

print()
print("=== 2) Server酱 微信推送 ===")
try:
    body = urllib.parse.urlencode({"title": "厨房助手 通道自检",
                                   "desp": "如果你在微信看到这条，说明推送通道正常。"}).encode()
    with urllib.request.urlopen("https://sctapi.ftqq.com/%s.send" % sc_key,
                                data=body, timeout=40,
                                context=ssl.create_default_context()) as r:
        print("HTTP", r.status, "->", r.read().decode("utf-8", "replace")[:300])
    print("结论: 推送 Key 可用（微信应已收到一条自检消息）")
except Exception as e:
    print("异常:", type(e).__name__, e)
    print("结论: ★ 推送不可用")
