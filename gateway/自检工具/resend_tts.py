# -*- coding: utf-8 -*-
"""把已生成的 test_tts.wav 重发 N 次（等板子开机联网）"""
import os, socket, struct, time

HERE = os.path.dirname(os.path.abspath(__file__))
WAV = os.path.join(HERE, "test_tts.wav")
TOPIC = "openvela/kitchen/device001/ai/response/audio"
wav = open(WAV, "rb").read()


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


for i in range(3):
    if i:
        time.sleep(40)
    try:
        cid = "ttsresend_%d_%d" % (int(time.time()), i)
        s = socket.create_connection(("broker.emqx.io", 1883), timeout=20)
        pl = b"\x00\x04MQTT\x04\x02\x00\x3c" + struct.pack(">H", len(cid)) + cid.encode()
        s.sendall(b"\x10" + remlen(len(pl)) + pl)
        readn(s, 1); rl = rd_remlen(s); readn(s, rl)

        t = TOPIC.encode()
        body = struct.pack(">H", len(t)) + t + struct.pack(">H", 90 + i) + wav
        s.sendall(b"\x32" + remlen(len(body)) + body)

        s.settimeout(15)
        try:
            h = readn(s, 1); rl = rd_remlen(s); readn(s, rl)
            print("[%d] 已发送 %d 字节, 应答 type=%d(PUBACK=%d)"
                  % (i + 1, len(wav), h[0] >> 4, 4), flush=True)
        except Exception as e:
            print("[%d] 已发送, 未收到 PUBACK (%s)" % (i + 1, type(e).__name__), flush=True)
        s.close()
    except Exception as e:
        print("[%d] 发送失败: %s" % (i + 1, e), flush=True)

print("三次发送结束")
