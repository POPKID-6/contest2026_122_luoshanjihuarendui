# -*- coding: utf-8 -*-
"""纯标准库：以设备身份发一条文字提问，并订阅下行，判断网关是否在正常工作。"""
import socket, struct, sys, time

HOST, PORT = "broker.emqx.io", 1883
DEV = "device001"
BASE = "openvela/kitchen/" + DEV
CID = "probe_%d" % int(time.time())


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


def rd_remlen(s):
    mult, val = 1, 0
    while True:
        b = s.recv(1)
        if not b:
            return None
        val += (b[0] & 0x7F) * mult
        mult *= 128
        if not (b[0] & 0x80):
            return val


def readn(s, n):
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            raise EOFError
        buf += c
    return buf


s = socket.create_connection((HOST, PORT), timeout=15)
pl = b"\x00\x04MQTT\x04\x02\x00\x3c" + struct.pack(">H", len(CID)) + CID.encode()
s.sendall(b"\x10" + remlen(len(pl)) + pl)
readn(s, 1); rl = rd_remlen(s); readn(s, rl)
print("已连上 broker", flush=True)

# 订阅下行
topics = [BASE + "/ai/response/text", BASE + "/cmd", BASE + "/state"]
body = struct.pack(">H", 1)
for t in topics:
    body += struct.pack(">H", len(t)) + t.encode() + b"\x00"
s.sendall(b"\x82" + remlen(len(body)) + body)
print("已订阅下行:", topics, flush=True)

# 发文字提问（QoS0，与设备一致）
text = "用一句话介绍你自己"
payload = ('{"type":"text","text":"%s"}' % text).encode("utf-8")
topic = BASE + "/ai/request"
pub = struct.pack(">H", len(topic)) + topic.encode() + payload
s.sendall(b"\x30" + remlen(len(pub)) + pub)
print("已发送提问:", text, flush=True)

t0 = time.time()
s.settimeout(40)
got = 0
while time.time() - t0 < 40:
    try:
        h = readn(s, 1)
    except (socket.timeout, EOFError):
        break
    rl = rd_remlen(s)
    if rl is None:
        break
    pkt = readn(s, rl) if rl else b""
    if (h[0] >> 4) != 3:
        continue
    qos = (h[0] & 0x06) >> 1
    tlen = struct.unpack(">H", pkt[:2])[0]
    tp = pkt[2:2 + tlen].decode("utf-8", "replace")
    off = 2 + tlen + (2 if qos > 0 else 0)
    data = pkt[off:]
    try:
        txt = data.decode("utf-8")[:200].replace("\n", " ")
    except Exception:
        txt = "<binary %d bytes>" % len(data)
    print("%6.1fs 下行 %-44s qos=%d len=%-7d %s" % (time.time() - t0, tp, qos, len(data), txt), flush=True)
    got += 1
    if tp.endswith("response/text"):
        break

print()
print("结论:", "★ 网关有回应，网关侧正常" if got else "★ 网关完全没有回应（网关没跑 / 没订阅 / 已卡死）", flush=True)
