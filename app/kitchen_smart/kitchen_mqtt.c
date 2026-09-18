/****************************************************************************
 * kitchen_mqtt.c - 轻量 MQTT 3.1.1 客户端（v2）
 *
 *  - broker/端口/设备ID 可由 /data/etc/kitchen.conf 配置
 *  - 后台线程自动重连（网络后连也能恢复）
 *  - 下行文本/指令交给 UI；下行音频（WAV）落盘后自动播放
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>

#include "kitchen_smart.h"

#define KS_KEEPALIVE  60
#define KS_RETRY_SECS 5

/* 单包上限：TTS 的 16k WAV 一秒 32KB，8 秒回答约 256KB，
 * 原来写死 128KB 会把语音包整个丢掉并导致 MQTT 流失步。 */
#define KS_MAX_PKT    (512 * 1024)

static char  g_host[64];
static int   g_port;
static char  g_dev[32];

static int           g_sock = -1;
static volatile bool g_run = false;
static volatile bool g_connected = false;
static uint16_t      g_pktid = 1;
static pthread_t     g_thread;
static bool          g_started = false;
static ks_on_text_t  g_on_text;
static ks_on_cmd_t   g_on_cmd;
static ks_on_audio_t g_on_audio;

/* 发送互斥：UI 线程、录音上传线程、MQTT 线程都可能写同一个 socket，
 * 不加锁会让 MQTT 报文互相穿插，broker 直接把连接踢掉。 */
static pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;

/* JSON 字符串转义：处理 " \ 换行 制表符，避免下行/上行报文被破坏 */
static void ks_json_escape(FAR const char *in, FAR char *out, int outlen)
{
  int o = 0;

  if (outlen <= 0)
    {
      return;
    }

  for (; in != NULL && *in != '\0' && o < outlen - 2; in++)
    {
      unsigned char c = (unsigned char)*in;
      if (c == '"' || c == '\\')
        {
          out[o++] = '\\';
          out[o++] = (char)c;
        }
      else if (c == '\n')
        {
          out[o++] = '\\';
          out[o++] = 'n';
        }
      else if (c == '\r')
        {
          out[o++] = '\\';
          out[o++] = 'r';
        }
      else if (c == '\t')
        {
          out[o++] = '\\';
          out[o++] = 't';
        }
      else if (c < 0x20)
        {
          out[o++] = ' ';
        }
      else
        {
          out[o++] = (char)c;
        }
    }

  out[o] = '\0';
}

void ks_mqtt_set_broker(FAR const char *host, int port, FAR const char *dev)
{
  strncpy(g_host, host ? host : CONFIG_KITCHEN_SMART_MQTT_HOST,
          sizeof(g_host) - 1);
  g_port = port > 0 ? port : CONFIG_KITCHEN_SMART_MQTT_PORT;
  strncpy(g_dev, dev ? dev : CONFIG_KITCHEN_SMART_DEVICE_ID,
          sizeof(g_dev) - 1);
  syslog(LOG_INFO, "kitchen: broker %s:%d dev=%s\n", g_host, g_port, g_dev);
}

void ks_mqtt_set_handlers(ks_on_text_t t, ks_on_cmd_t c, ks_on_audio_t a)
{
  g_on_text = t;
  g_on_cmd = c;
  g_on_audio = a;
}

bool ks_mqtt_connected(void)
{
  return g_connected;
}

static int put_u16(FAR uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xff);
  return 2;
}

static int put_remlen(FAR uint8_t *p, int len)
{
  int n = 0;
  do
    {
      uint8_t d = len % 128;
      len /= 128;
      if (len > 0)
        {
          d |= 0x80;
        }

      p[n++] = d;
    }
  while (len > 0);
  return n;
}

static int write_all(int fd, FAR const void *buf, int len)
{
  FAR const uint8_t *p = buf;
  int off = 0;
  while (off < len)
    {
      int w = write(fd, p + off, len - off);
      if (w > 0)
        {
          off += w;
          continue;
        }

      /* 被信号打断 / 发送缓冲暂时满：重试，别把成功的发送误判为失败 */
      if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
          usleep(20 * 1000);
          continue;
        }

      return -1;
    }

  return 0;
}

static int read_n(int fd, FAR void *buf, int len)
{
  uint8_t *p = buf;
  int off = 0;
  while (off < len)
    {
      int r = read(fd, p + off, len - off);
      if (r <= 0)
        {
          return -1;
        }

      off += r;
    }

  return off;
}

static int mqtt_handshake(int fd)
{
  uint8_t pkt[512];
  int pos = 0;
  int cidlen = strlen(g_dev);
  int pl = 2 + 4 + 1 + 1 + 2 + 2 + cidlen;

  pkt[pos++] = 0x10;
  pos += put_remlen(pkt + pos, pl);
  pos += put_u16(pkt + pos, 4);
  memcpy(pkt + pos, "MQTT", 4);
  pos += 4;
  pkt[pos++] = 4;
  pkt[pos++] = 0x02;
  pos += put_u16(pkt + pos, KS_KEEPALIVE);
  pos += put_u16(pkt + pos, cidlen);
  memcpy(pkt + pos, g_dev, cidlen);
  pos += cidlen;

  if (write_all(fd, pkt, pos) != 0)
    {
      return -1;
    }

  uint8_t hdr[4];
  int remlen = 0, mult = 1, i, rc;
  uint8_t body[16];

  /* 读 CONNACK：0x20 + 剩余长度 + 内容（原来写死读 4 字节，不健壮） */
  if (read_n(fd, hdr, 1) != 1 || hdr[0] != 0x20)
    {
      return -1;
    }

  for (i = 0; i < 4; i++)
    {
      if (read_n(fd, &hdr[0], 1) != 1)
        {
          return -1;
        }

      remlen += (hdr[0] & 0x7f) * mult;
      mult *= 128;
      if ((hdr[0] & 0x80) == 0)
        {
          break;
        }
    }

  if (remlen < 2 || remlen > (int)sizeof(body))
    {
      return -1;
    }

  if (read_n(fd, body, remlen) != remlen)
    {
      return -1;
    }

  rc = body[1];
  if (rc != 0)
    {
      ks_ui_log("MQTT 被拒绝 rc=%d", rc);
      return -1;
    }

  return 0;
}

static int mqtt_subscribe(int fd, FAR const char *topic, uint16_t id)
{
  uint8_t pkt[300];
  int pos = 0;
  int tlen = strlen(topic);
  int pl = 2 + 2 + tlen + 1;

  pkt[pos++] = 0x82;
  pos += put_remlen(pkt + pos, pl);
  pos += put_u16(pkt + pos, id);
  pos += put_u16(pkt + pos, tlen);
  memcpy(pkt + pos, topic, tlen);
  pos += tlen;
  pkt[pos++] = 0;          /* requested QoS 0 */

  return write_all(fd, pkt, pos);
}

/* 等 SUBACK：MQTT 规定同一个报文 ID 在确认前不能复用，
 * 三个订阅都用 ID=1 时 broker 可能只接受第一个（音频/cmd 就永远收不到）。 */
static void mqtt_wait_suback(int fd, int count)
{
  int got = 0;
  int guard = 0;

  while (got < count && guard++ < 12)
    {
      uint8_t b;
      int rl = 0, mult = 1, i;
      uint8_t body[64];

      if (read_n(fd, &b, 1) != 1)
        {
          return;
        }

      for (i = 0; i < 4; i++)
        {
          uint8_t c;
          if (read_n(fd, &c, 1) != 1)
            {
              return;
            }

          rl += (c & 0x7f) * mult;
          mult *= 128;
          if ((c & 0x80) == 0)
            {
              break;
            }
        }

      if (rl > 0 && rl <= (int)sizeof(body))
        {
          if (read_n(fd, body, rl) != rl)
            {
              return;
            }
        }
      else if (rl > 0)
        {
          uint8_t sink[256];
          int left = rl;

          while (left > 0)
            {
              int n = (left > (int)sizeof(sink)) ? (int)sizeof(sink) : left;
              if (read_n(fd, sink, n) != n)
                {
                  return;
                }

              left -= n;
            }
        }

      if ((b >> 4) == 9)          /* SUBACK */
        {
          got++;
          if (rl >= 2)
            {
              syslog(LOG_INFO, "kitchen: suback id=%d qos=%d\n",
                     (body[0] << 8) | body[1], body[2]);
            }
        }
    }
}

static int mqtt_publish_raw(int fd, FAR const char *topic,
                            FAR const void *payload, int len, int qos)
{
  uint8_t hdr[8];
  uint8_t varbuf[300];
  int varlen = 0;
  int tlen = strlen(topic);
  int hpos;
  int ret;

  varlen += put_u16(varbuf + varlen, tlen);
  memcpy(varbuf + varlen, topic, tlen);
  varlen += tlen;
  if (qos > 0)
    {
      /* 调用方已持锁，这里直接自增即可（保证 ID 唯一） */
      g_pktid++;
      if (g_pktid == 0)
        {
          g_pktid = 1;
        }

      varlen += put_u16(varbuf + varlen, g_pktid);
    }

  hdr[0] = 0x30 | (qos > 0 ? 0x02 : 0x00);
  hpos = 1 + put_remlen(hdr + 1, varlen + len);

  /* 调用方必须已持有 g_tx_lock（本函数把 3 次写当成一个原子单元） */
  ret = (write_all(fd, hdr, hpos) != 0 ||
         write_all(fd, varbuf, varlen) != 0 ||
         (len > 0 && write_all(fd, payload, len) != 0)) ? -1 : 0;

  return ret;
}

/* 下行音频：落盘 + 播放（新线程，避免阻塞收包） */
static void *play_thread(FAR void *arg)
{
  FAR const char *path = (arg != NULL) ? (FAR const char *)arg : KS_TTS_WAV;
  ks_audio_play(path);
  return NULL;
}

/* TTS 文件用 A/B 两个槽位轮换：避免新音频覆盖正在播放的文件 */
static int g_tts_slot = 0;

static FAR const char *next_tts_path(void)
{
  g_tts_slot ^= 1;
  return g_tts_slot ? KS_TTS_WAV_B : KS_TTS_WAV_A;
}

/* 处理下行 PUBLISH。
 * qos>0 时变长头里 topic 之后还有 2 字节 packet id（必须跳过，
 * 否则载荷前会多 2 个垃圾字节、PUBACK 也会回错 ID 导致 broker 疯狂重发）。
 * 返回值：>=0 需要回 PUBACK 的报文 ID；-1 无法处理/不需要 ACK。 */
static int handle_downlink(int fd, FAR const uint8_t *payload, int plen, int qos)
{
  char topic[128];
  int tlen, off = 2;
  int pktid = -1;

  if (plen < 2)
    {
      return -1;
    }

  tlen = (payload[0] << 8) | payload[1];
  if (off + tlen > plen || tlen >= (int)sizeof(topic) - 1)
    {
      return -1;                       /* 主题异常：整包丢弃 */
    }

  memcpy(topic, payload + off, tlen);
  topic[tlen] = '\0';
  off += tlen;

  if (qos > 0)
    {
      if (off + 2 > plen)
        {
          return -1;
        }

      pktid = (payload[off] << 8) | payload[off + 1];
      off += 2;
    }

  FAR const uint8_t *body = payload + off;
  int blen = plen - off;
  char expect[128];

  snprintf(expect, sizeof(expect), KS_TOPIC_RESP_TEXT, g_dev);
  if (strcmp(topic, expect) == 0)
    {
      if (g_on_text)
        {
          g_on_text((FAR const char *)body, blen);
        }

      return pktid;
    }

  snprintf(expect, sizeof(expect), KS_TOPIC_CMD, g_dev);
  if (strcmp(topic, expect) == 0)
    {
      if (g_on_cmd)
        {
          g_on_cmd((FAR const char *)body, blen);
        }

      return pktid;
    }

  snprintf(expect, sizeof(expect), KS_TOPIC_RESP_AUD, g_dev);
  if (strcmp(topic, expect) == 0)
    {
      if (g_on_audio)
        {
          g_on_audio(body, blen);
        }

      /* WAV 落盘并播放（A/B 槽位轮换，不覆盖正在播的那一份） */
      FAR const char *wpath = next_tts_path();
      FILE *f = fopen(wpath, "wb");
      if (f != NULL)
        {
          fwrite(body, 1, blen, f);
          fclose(f);

          /* 另存一份去掉 WAV 头的裸 PCM（播放端也会重新提取一次，这里做冗余备份） */
          int off2 = 12;                    /* 跳过 "RIFF" + size + "WAVE" */

          while (off2 + 8 <= blen)
            {
              int csize = (int)((unsigned int)body[off2 + 4] |
                                ((unsigned int)body[off2 + 5] << 8) |
                                ((unsigned int)body[off2 + 6] << 16) |
                                ((unsigned int)body[off2 + 7] << 24));

              if (body[off2] == 'd' && body[off2 + 1] == 'a' &&
                  body[off2 + 2] == 't' && body[off2 + 3] == 'a')
                {
                  int dstart = off2 + 8;

                  /* 云端 data size 常是垃圾大值，越界/非法时按剩余长度兜底 */
                  int dlen = (csize > 0 && dstart + csize <= blen)
                             ? csize : (blen - dstart);

                  if (dlen > 0)
                    {
                      FILE *g = fopen(KS_TTS_PCM, "wb");
                      if (g != NULL)
                        {
                          fwrite(body + dstart, 1, dlen, g);
                          fclose(g);
                        }
                    }

                  break;
                }

              /* 非 data 块：大小异常就放弃解析（避免 off2 回绕越界） */
              if (csize < 0 || off2 + 8 + csize > blen)
                {
                  break;
                }

              off2 += 8 + csize + (csize & 1);
            }

          pthread_t tid;
          pthread_attr_t attr;

          /* detached：播放线程退出后自动回收，不然每次 TTS 都漏一份栈 */
          pthread_attr_init(&attr);
          pthread_attr_setstacksize(&attr, 12288);
          pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
          if (pthread_create(&tid, &attr, play_thread,
                             (FAR void *)wpath) != 0)
            {
              ks_ui_log("TTS 播放线程创建失败");
            }

          pthread_attr_destroy(&attr);
        }
      else
        {
          ks_ui_log("TTS 写文件失败(%d 字节)", blen);
        }

      return pktid;
    }

  return pktid;
}

/* 断开连接并标记（多处复用） */
static void mqtt_drop_conn(void)
{
  g_connected = false;
  if (g_sock >= 0)
    {
      close(g_sock);
      g_sock = -1;
    }
}

/* 起一个 detached 播放线程 */
static void spawn_play_thread(FAR const char *path)
{
  pthread_t tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 12288);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, play_thread, (FAR void *)path) != 0)
    {
      ks_ui_log("TTS 播放线程创建失败");
    }

  pthread_attr_destroy(&attr);
}

static int mqtt_connect_once(void)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -1;
    }

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(g_port);

  /* 域名解析：broker.emqx.io 这类主机名用 inet_addr() 会直接失败，
   * 之前就是这里导致设备永远连不上 broker。 */
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  char portstr[8];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portstr, sizeof(portstr), "%d", g_port);

  if (getaddrinfo(g_host, portstr, &hints, &res) == 0 && res != NULL)
    {
      memcpy(&sa, res->ai_addr, sizeof(sa));
      freeaddrinfo(res);
    }
  else
    {
      sa.sin_addr.s_addr = inet_addr(g_host);   /* 退回点分十进制 */
      if (sa.sin_addr.s_addr == INADDR_NONE)
        {
          ks_ui_log("DNS 解析失败: %s", g_host);
          close(fd);
          return -1;
        }
    }

  struct timeval tv;
  tv.tv_sec = 20;              /* 大包（170KB 音频）中途允许更长空档 */
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* 发送超时：否则链路拥塞时 write 会卡住 UI 线程几十秒（按告警时整机假死） */
  tv.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(fd, (FAR struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
      close(fd);
      return -1;
    }

  if (mqtt_handshake(fd) != 0)
    {
      close(fd);
      return -1;
    }

  char t[128];
  snprintf(t, sizeof(t), KS_TOPIC_RESP_TEXT, g_dev);
  mqtt_subscribe(fd, t, 1);
  snprintf(t, sizeof(t), KS_TOPIC_CMD, g_dev);
  mqtt_subscribe(fd, t, 2);
  snprintf(t, sizeof(t), KS_TOPIC_RESP_AUD, g_dev);
  mqtt_subscribe(fd, t, 3);

  /* 等三条 SUBACK，确认三个主题都订阅成功（否则收不到音频/cmd） */
  mqtt_wait_suback(fd, 3);

  g_sock = fd;
  g_connected = true;
  syslog(LOG_INFO, "kitchen: mqtt connected %s:%d\n", g_host, g_port);
  ks_ui_log("MQTT connected");
  return 0;
}

static void *mqtt_thread(FAR void *arg)
{
  (void)arg;
  int idle_ticks = 0;        /* 连续空闲次数（每次读超时约 5s） */
  int ping_out = 0;          /* 已发出但未收到 PINGRESP 的次数 */

  while (g_run)
    {
      if (!g_connected)
        {
          if (mqtt_connect_once() != 0)
            {
              sleep(KS_RETRY_SECS);
              continue;
            }

          idle_ticks = 0;
          ping_out = 0;
        }

      /* 收包循环：用 poll() 保证每 5 秒必定回到这里。
       * 只依赖 SO_RCVTIMEO 时，某些驱动/配置下 read 会一直阻塞，
       * 结果心跳永远发不出去、连接被 broker 静默踢掉（发出去的数据没人收）。 */
      uint8_t hdr[5];
      struct pollfd pfd;
      int pr;
      int r;

      pfd.fd = g_sock;
      pfd.events = POLLIN;
      pfd.revents = 0;

      pr = poll(&pfd, 1, 5000);
      if (pr == 0)
        {
          r = 0;                       /* 超时：没有下行数据 */
        }
      else if (pr < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          r = -1;
        }
      else if (read_n(g_sock, hdr, 1) == 1)
        {
          r = 1;
        }
      else
        {
          r = -1;
        }

      if (r == 0)
        {
          /* 注意：这里不用 time()，因为板子 RTC 未初始化时时间可能不前进，
           * 会导致心跳永不发出、broker 静默踢连接（数据发出去没人收）。 */
          idle_ticks++;

          if (ping_out >= 3)
            {
              ks_ui_log("MQTT 无响应，强制重连");
              g_connected = false;
              close(g_sock);
              g_sock = -1;
              ping_out = 0;
              idle_ticks = 0;
              sleep(KS_RETRY_SECS);
              continue;
            }

          if (idle_ticks >= 6)          /* 约 30 秒（读超时 5s × 6） */
            {
              uint8_t ping[2] = { 0xc0, 0x00 };
              int perr;

              pthread_mutex_lock(&g_tx_lock);
              perr = write_all(g_sock, ping, 2);
              pthread_mutex_unlock(&g_tx_lock);

              if (perr != 0)
                {
                  g_connected = false;
                  close(g_sock);
                  g_sock = -1;
                }
              else
                {
                  ping_out++;
                }

              idle_ticks = 0;
            }

          continue;
        }

      if (r < 0)
        {
          g_connected = false;
          close(g_sock);
          g_sock = -1;
          ks_ui_log("MQTT 断开，重连中...");
          sleep(KS_RETRY_SECS);
          continue;
        }

      uint8_t type = hdr[0] >> 4;
      int mult = 1;
      int remlen = 0;
      int bad = 0;
      for (int i = 0; i < 4; i++)
        {
          uint8_t b;
          if (read_n(g_sock, &b, 1) != 1)
            {
              bad = 1;
              break;
            }

          remlen += (b & 0x7f) * mult;
          mult *= 128;
          if ((b & 0x80) == 0)
            {
              break;
            }
        }

      if (bad || remlen < 0)
        {
          g_connected = false;
          close(g_sock);
          g_sock = -1;
          continue;
        }

      if (remlen == 0)
        {
          continue;
        }

      /* 大包（TTS 音频约 170KB）：流式落盘，
       * 避免一次性 malloc 上百 KB 失败就被整个丢掉（表现为"没声音"） */
      if (remlen > (64 * 1024))
        {
          uint8_t head[320];
          char tbuf[256];
          char atopic[128];
          int tlen, hlen, is_audio, pid2 = -1;
          long left, written = 0;
          FILE *fw = NULL;
          FILE *fp = NULL;
          FAR const char *wpath = NULL;
          uint8_t chunk[2048];

          if (read_n(g_sock, head, 2) != 2)
            {
              mqtt_drop_conn();
              sleep(KS_RETRY_SECS);
              continue;
            }

          tlen = (head[0] << 8) | head[1];
          /* 上限按 tbuf 容量算（原来按 head 算，最多可越界写 tbuf 62 字节） */
          if (tlen <= 0 || tlen > (int)sizeof(tbuf) - 1 ||
              read_n(g_sock, head + 2, tlen) != tlen)
            {
              mqtt_drop_conn();
              sleep(KS_RETRY_SECS);
              continue;
            }

          memcpy(tbuf, head + 2, tlen);
          tbuf[tlen] = '\0';
          hlen = 2 + tlen;

          if ((hdr[0] & 0x06) == 0x02)
            {
              uint8_t pid[2];

              if (read_n(g_sock, pid, 2) != 2)
                {
                  mqtt_drop_conn();
                  sleep(KS_RETRY_SECS);
                  continue;
                }

              pid2 = (pid[0] << 8) | pid[1];
              hlen += 2;
            }

          snprintf(atopic, sizeof(atopic), KS_TOPIC_RESP_AUD, g_dev);
          is_audio = (strcmp(tbuf, atopic) == 0);

          /* 单包上限：超过 1MB 的一律丢弃（只排空不落盘），
           * 否则异常重发能把 /tmp(tmpfs) 写满 */
          if (is_audio && remlen > (1024 * 1024))
            {
              ks_ui_log("音频包过大(%d)，已丢弃", remlen);
              is_audio = 0;
            }

          if (is_audio)
            {
              wpath = next_tts_path();
              fw = fopen(wpath, "wb");
            }

          left = remlen - hlen;

          while (left > 0)
            {
              int want = (left > (long)sizeof(chunk))
                         ? (int)sizeof(chunk) : (int)left;

              if (read_n(g_sock, chunk, want) != want)
                {
                  if (fw != NULL) fclose(fw);
                  if (fp != NULL) fclose(fp);
                  mqtt_drop_conn();
                  sleep(KS_RETRY_SECS);
                  break;
                }

              if (fw != NULL)
                {
                  fwrite(chunk, 1, want, fw);

                  /* 同时写一份去掉 44 字节 WAV 头的裸 PCM（播放兜底） */
                  if (written + want > 44)
                    {
                      int start = (written < 44) ? (int)(44 - written) : 0;

                      if (fp == NULL)
                        {
                          fp = fopen(KS_TTS_PCM, "wb");
                        }

                      if (fp != NULL)
                        {
                          fwrite(chunk + start, 1, want - start, fp);
                        }
                    }
                }

              written += want;
              left -= want;
            }

          if (fp != NULL) fclose(fp);
          if (fw != NULL) fclose(fw);

          if (left > 0)
            {
              continue;                 /* 中途读失败，已经断开 */
            }

          if (pid2 >= 0)
            {
              uint8_t ack[4] = { 0x40, 0x02, (uint8_t)(pid2 >> 8),
                                 (uint8_t)(pid2 & 0xff) };

              pthread_mutex_lock(&g_tx_lock);
              write_all(g_sock, ack, 4);
              pthread_mutex_unlock(&g_tx_lock);
            }

          if (is_audio)
            {
              ks_ui_log("TTS %ld 字节，开始播放", written);
              spawn_play_thread(wpath);
            }

          idle_ticks = 0;
          continue;
        }

      if (remlen > KS_MAX_PKT)
        {
          /* 超过上限（例如超长 TTS）：必须把字节读完，否则流永久错位 */
          int left = remlen;
          uint8_t sink[1024];

          while (left > 0)
            {
              int chunk = (left > (int)sizeof(sink)) ? (int)sizeof(sink) : left;
              if (read_n(g_sock, sink, chunk) != chunk)
                {
                  g_connected = false;
                  close(g_sock);
                  g_sock = -1;
                  break;
                }

              left -= chunk;
            }

          continue;
        }

      uint8_t *body = malloc(remlen);
      if (body == NULL)
        {
          /* 分配失败也要把包体读掉，保持流同步 */
          int left = remlen;
          uint8_t sink[512];

          while (left > 0)
            {
              int chunk = (left > (int)sizeof(sink)) ? (int)sizeof(sink) : left;
              if (read_n(g_sock, sink, chunk) != chunk)
                {
                  g_connected = false;
                  close(g_sock);
                  g_sock = -1;
                  break;
                }

              left -= chunk;
            }

          continue;
        }

      if (read_n(g_sock, body, remlen) != remlen)
        {
          free(body);
          g_connected = false;
          close(g_sock);
          g_sock = -1;
          continue;
        }

      if (type == 3)
        {
          int qos = (hdr[0] & 0x06) >> 1;
          int pktid = handle_downlink(g_sock, body, remlen, qos);

          /* 先回 PUBACK（正确 ID）再让上层处理，避免 broker 超时重发 */
          if (qos > 0 && pktid >= 0)
            {
              uint8_t ack[4] = { 0x40, 0x02,
                                 (uint8_t)(pktid >> 8), (uint8_t)(pktid & 0xff) };

              pthread_mutex_lock(&g_tx_lock);
              write_all(g_sock, ack, 4);
              pthread_mutex_unlock(&g_tx_lock);
            }
        }
      else if (type == 12)
        {
          uint8_t rsp[2] = { 0xd0, 0x00 };
          pthread_mutex_lock(&g_tx_lock);
          write_all(g_sock, rsp, 2);
          pthread_mutex_unlock(&g_tx_lock);
        }
      else if (type == 13)
        {
          ping_out = 0;           /* 收到 PINGRESP，连接确认存活 */
        }

      idle_ticks = 0;
      free(body);
    }

  return NULL;
}

int ks_mqtt_start(void)
{
  if (g_started)
    {
      return 0;
    }

  if (g_host[0] == '\0')
    {
      ks_mqtt_set_broker(CONFIG_KITCHEN_SMART_MQTT_HOST,
                         CONFIG_KITCHEN_SMART_MQTT_PORT,
                         CONFIG_KITCHEN_SMART_DEVICE_ID);
    }

  g_run = true;
  g_started = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 12288);
  return pthread_create(&g_thread, &attr, mqtt_thread, NULL);
}

void ks_mqtt_stop(void)
{
  g_run = false;
  if (g_sock >= 0)
    {
      close(g_sock);
      g_sock = -1;
    }

  g_connected = false;
  g_started = false;      /* 清零后 ks_mqtt_start() 才能真正重新建线程 */
}

int ks_mqtt_publish(FAR const char *topic, FAR const void *payload,
                    int len, int qos)
{
  int rc;

  /* 判活与写入必须同锁：否则 ks_mqtt_stop/重连关掉 fd 后，
   * 这一包可能被写进号码复用的无关 fd（文件/音频设备）。 */
  pthread_mutex_lock(&g_tx_lock);
  if (!g_connected || g_sock < 0)
    {
      pthread_mutex_unlock(&g_tx_lock);
      return -ENOTCONN;
    }

  rc = mqtt_publish_raw(g_sock, topic, payload, len, qos);
  pthread_mutex_unlock(&g_tx_lock);
  return rc;
}

int ks_mqtt_publish_ai_text(FAR const char *text)
{
  char topic[128];
  char js[512];
  char esc[384];
  int n;

  ks_json_escape(text, esc, sizeof(esc));
  snprintf(topic, sizeof(topic), KS_TOPIC_REQ, g_dev);
  n = snprintf(js, sizeof(js), "{\"type\":\"text\",\"text\":\"%s\"}", esc);
  if (n <= 0)
    {
      return -EINVAL;
    }

  return ks_mqtt_publish(topic, js, n, 1);
}

int ks_mqtt_publish_ai_pcm(FAR const void *pcm, int len)
{
  char topic[128];
  snprintf(topic, sizeof(topic), KS_TOPIC_REQ, g_dev);
  return ks_mqtt_publish(topic, pcm, len, 1);
}

/* 把一段文本（例如 AI 的回答）转发到用户微信：网关收到 type=wechat 后推送 */
int ks_mqtt_publish_wechat(FAR const char *text)
{
  char topic[128];
  char js[768];
  char esc[640];
  int n;

  ks_json_escape(text, esc, sizeof(esc));
  snprintf(topic, sizeof(topic), KS_TOPIC_REQ, g_dev);
  n = snprintf(js, sizeof(js), "{\"type\":\"wechat\",\"text\":\"%s\"}", esc);
  if (n <= 0)
    {
      return -EINVAL;
    }

  return ks_mqtt_publish(topic, js, n, 1);
}

int ks_mqtt_publish_alert(int temp_x10, int hum_x10, int risk,
                          FAR const char *reason)
{
  char topic[128];
  char js[512];
  char esc[192];
  int n;
  int tf, hf;

  /* 小数点后取绝对值：temp_x10 为负时 %d.%d 会拼出 "0.-5" 这种非法数字 */
  tf = temp_x10 % 10;
  if (tf < 0)
    {
      tf = -tf;
    }

  hf = hum_x10 % 10;
  if (hf < 0)
    {
      hf = -hf;
    }

  ks_json_escape(reason ? reason : "", esc, sizeof(esc));

  snprintf(topic, sizeof(topic), KS_TOPIC_ALERT, g_dev);
  n = snprintf(js, sizeof(js),
               "{\"temp\":%d.%d,\"hum\":%d.%d,\"risk\":%d,"
               "\"reason\":\"%s\",\"ts\":%ld}",
               temp_x10 / 10, tf, hum_x10 / 10, hf, risk, esc,
               (long)time(NULL));
  if (n <= 0)
    {
      return -EINVAL;
    }

  int ret = ks_mqtt_publish(topic, js, n, 1);

  snprintf(topic, sizeof(topic), KS_TOPIC_STATE, g_dev);
  ks_mqtt_publish(topic, js, n, 0);
  return ret;
}
