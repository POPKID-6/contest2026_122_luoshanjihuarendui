# -*- coding: utf-8 -*-
"""纯标准库 MQTT 3.1.1 嗅探器：订阅 openvela/kitchen/device001/#，只打印不发送。
用来判断：设备的上行到底有没有到 broker、网关有没有回。"""
import socket, struct, sys, time

LOGF = r"D:\openvela\cloud_gateway\sniff.log"


def log(line):
    print(line, flush=True)
    try:
        with open(LOGF, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass

HOST, PORT = "broker.emqx.io", 1883
TOPIC = "openvela/kitchen/#"
CID = "sniff_%d" % int(time.time())


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
payload = b"\x00\x04MQTT\x04\x02\x00\x3c" + struct.pack(">H", len(CID)) + CID.encode()
s.sendall(b"\x10" + remlen(len(payload)) + payload)
hdr = readn(s, 1)
rl = rd_remlen(s)
body = readn(s, rl)
print("CONNACK:", body.hex(), "(rc=%d)" % body[1] if len(body) > 1 else "", flush=True)

sub = struct.pack(">H", 1) + struct.pack(">H", len(TOPIC)) + TOPIC.encode() + b"\x01"
s.sendall(b"\x82" + remlen(len(sub)) + sub)
print("SUBSCRIBED:", TOPIC, flush=True)
print("---- 现在请在板子上点 Send 或 Rec 3s ----", flush=True)

t0 = time.time()
s.settimeout(1800)
while time.time() - t0 < 1800:
    try:
        h = readn(s, 1)
    except (socket.timeout, EOFError):
        break
    rl = rd_remlen(s)
    if rl is None:
        break
    pkt = readn(s, rl) if rl else b""
    typ = h[0] >> 4
    if typ == 3:
        qos = (h[0] & 0x06) >> 1
        tlen = struct.unpack(">H", pkt[:2])[0]
        topic = pkt[2:2 + tlen].decode("utf-8", "replace")
        off = 2 + tlen + (2 if qos > 0 else 0)
        data = pkt[off:]
        try:
            txt = data.decode("utf-8")[:100].replace("\n", " ")
        except Exception:
            txt = "<binary>"
        log("%7.1fs  RECV qos=%d  %-46s len=%-7d %s"
            % (time.time() - t0, qos, topic, len(data), txt))
    elif typ == 9:
        log("%7.1fs  SUBACK" % (time.time() - t0))
    elif typ == 13:
        log("%7.1fs  PINGRESP" % (time.time() - t0))

print("sniffer 结束", flush=True)
