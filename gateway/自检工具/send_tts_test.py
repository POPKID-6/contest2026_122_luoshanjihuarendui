# -*- coding: utf-8 -*-
"""生成一段真实 TTS 语音并直接发到设备的音频下行主题，用于隔离验证：
- 若设备状态行出现 "TTS ... 开始播放" -> 大包收得到
- 若有声音 -> 播放链路 OK
"""
import os
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY_DIR = os.path.dirname(HERE)
sys.path.insert(0, GATEWAY_DIR)
import gateway as gw          # noqa: E402

HOST, PORT = gw.MQTT_HOST, gw.MQTT_PORT
TOPIC = "openvela/kitchen/%s/ai/response/audio" % gw.DEV
CID = "ttstest_%d" % int(time.time())

TEXT = sys.argv[1] if len(sys.argv) > 1 else "你好，我是你的厨房助手，现在可以听到我说话吗？"


def remlen(n):
    out = b""
    while True:
        d = n % 128
        n //= 128
        if n > 0:
            d |= 0x80
        out += bytes([d])
        if n == 0:
            return out


def readn(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def rd_remlen(s):
    mult, val = 1, 0
    while True:
        c = s.recv(1)
        if not c:
            return None
        val += (c[0] & 0x7F) * mult
        mult *= 128
        if not (c[0] & 0x80):
            return val


print("1) 生成 TTS 语音 ...")
wav = gw.tts_bytes(TEXT)
if not wav:
    print("生成失败（检查 dashscope / Key）")
    raise SystemExit(1)
print("   WAV %d 字节, 前 4 字节=%s" % (len(wav), wav[:4]))
with open(os.path.join(HERE, "test_tts.wav"), "wb") as f:
    f.write(wav)

print("2) 连接 broker 并发送（QoS1，与网关一致）...")
s = socket.create_connection((HOST, PORT), timeout=20)
pl = b"\x00\x04MQTT\x04\x02\x00\x3c" + struct.pack(">H", len(CID)) + CID.encode()
s.sendall(b"\x10" + remlen(len(pl)) + pl)
readn(s, 1); rl = rd_remlen(s); readn(s, rl)

t = TOPIC.encode()
body = struct.pack(">H", len(t)) + t + struct.pack(">H", 99) + wav
s.sendall(b"\x32" + remlen(len(body)) + body)
print("   已发送 -> %s (%d 字节)" % (TOPIC, len(wav)))

# 等 PUBACK，确认 broker 收下了
s.settimeout(15)
try:
    h = readn(s, 1)
    rl = rd_remlen(s)
    pkt = readn(s, rl) if rl else b""
    print("   收到应答: type=%d %s" % (h[0] >> 4, "PUBACK" if (h[0] >> 4) == 4 else pkt.hex()))
except Exception as e:
    print("   未收到 PUBACK:", type(e).__name__)

s.close()
print("完成 —— 请看板子屏幕状态行 / 听有没有声音")
