/****************************************************************************
 * kitchen_smart.c - openvela 智能厨房终端 v2（Gemini-S1 / 2.8" SPI）
 *
 * 页面：首页(状态) / WiFi 设置 / AI 语音 / 微信告警
 *   - 每个子页面左上角都有 [< Back] 返回键，可自由进出
 *   - WiFi 页：扫描列表点选 + 软键盘输入 SSID/密码 + 连接
 *   - AI 页：软键盘文字提问 / 录音3s上传（云端 ASR+LLM） / 显示回答（中文）
 *   - 告警页：手动触发干烧告警 -> 云端 AI 建议 + 微信推送
 *   - 串口兜底：kitchen_smart ask "问题" | rec 5 | alert | status | wifi SSID PWD
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <lvgl/lvgl.h>

#include "kitchen_smart.h"

/* ---------------- 全局状态 ---------------- */
volatile int g_ks_risk = 0;
volatile int g_ks_temp_x10 = 950;
volatile int g_ks_hum_x10 = 600;

static struct ks_cfg_s g_cfg;

/* 跨线程消息缓冲（UI 定时器读取并显示，避免非 UI 线程碰 LVGL） */
static char g_log_buf[256];
static char g_ai_buf[1024];
static char g_wifi_buf[256];
static char g_alert_buf[256];

/* 扫描结果 */
static char g_ap[KS_WIFI_MAX_AP][KS_AP_NAME_LEN];
static volatile int g_ap_count = 0;
static volatile int g_scanning = 0;   /* 防连点：同一时刻只允许一个扫描线程 */
static volatile int g_scan_done = 0;  /* 扫描结束（含 0 个结果），通知 UI 刷新 */

/* 屏幕与控件 */
static lv_obj_t *scr_home, *scr_wifi, *scr_scan, *scr_ai, *scr_alert;
static lv_obj_t *lbl_home_status;
static lv_obj_t *lbl_wifi_status, *ta_ssid, *ta_pwd, *kb_wifi;
static lv_obj_t *list_scan, *lbl_scan_status;
static lv_obj_t *lbl_ai_text, *ta_ask, *kb_ai, *lbl_ai_status;
static lv_obj_t *lbl_alert_state;
static lv_font_t *g_font_cn;

/* 实际屏幕尺寸：本板是 ILI9341 横屏 320x240（不是 240x320！）
 * 所有布局都按这个尺寸算，避免控件互相覆盖。 */
static int g_scr_w = 320;
static int g_scr_h = 240;

/* 等待云端回答的状态：超过 20 秒就在界面上直接提示检查网关。
 * 用 tick 计数而不是 time()，避免板子 RTC 未初始化时时间不前进。 */
static volatile int g_waiting = 0;
static volatile int g_wait_ticks = 0;

static void hide_kb(void);      /* 前置声明：按钮回调里要用 */

static void scr_probe(void)
{
  lv_display_t *d = lv_display_get_default();
  if (d != NULL)
    {
      int w = (int)lv_display_get_horizontal_resolution(d);
      int h = (int)lv_display_get_vertical_resolution(d);
      if (w > 0 && h > 0)
        {
          g_scr_w = w;
          g_scr_h = h;
        }
    }

  syslog(LOG_INFO, "kitchen: screen %dx%d\n", g_scr_w, g_scr_h);
}

/* ---------------- 日志 ---------------- */
void ks_ui_log(FAR const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_log_buf, sizeof(g_log_buf), fmt, ap);
  va_end(ap);
  syslog(LOG_INFO, "kitchen: %s\n", g_log_buf);
}

/* ---------------- 配置读取 ---------------- */
void ks_cfg_defaults(FAR struct ks_cfg_s *c)
{
  memset(c, 0, sizeof(*c));
  strncpy(c->mqtt_host, CONFIG_KITCHEN_SMART_MQTT_HOST, sizeof(c->mqtt_host) - 1);
  c->mqtt_port = CONFIG_KITCHEN_SMART_MQTT_PORT;
  strncpy(c->device_id, CONFIG_KITCHEN_SMART_DEVICE_ID, sizeof(c->device_id) - 1);
  strncpy(c->wifi_if, CONFIG_KITCHEN_SMART_WIFI_IFNAME, sizeof(c->wifi_if) - 1);
}

int ks_cfg_load(FAR struct ks_cfg_s *c)
{
  FILE *f;
  char line[160];
  int n = 0;

  ks_cfg_defaults(c);

  f = fopen(KS_APP_CONF_PATH, "r");
  if (f == NULL)
    {
      syslog(LOG_INFO, "kitchen: no %s, use defaults\n", KS_APP_CONF_PATH);
      return -1;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      char *p = line;
      char *eq;
      while (*p == ' ' || *p == '\t')
        {
          p++;
        }

      if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\0')
        {
          continue;
        }

      eq = strchr(p, '=');
      if (eq == NULL)
        {
          continue;
        }

      *eq = '\0';
      char *k = p;
      char *v = eq + 1;
      char *e = v + strlen(v);
      while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
        {
          *--e = '\0';
        }

      char *ks = k + strlen(k);
      while (ks > k && (ks[-1] == ' ' || ks[-1] == '\t'))
        {
          *--ks = '\0';
        }

      if (strcmp(k, "mqtt_host") == 0 || strcmp(k, "host") == 0)
        {
          strncpy(c->mqtt_host, v, sizeof(c->mqtt_host) - 1);
          n++;
        }
      else if (strcmp(k, "mqtt_port") == 0 || strcmp(k, "port") == 0)
        {
          c->mqtt_port = atoi(v);
          n++;
        }
      else if (strcmp(k, "device_id") == 0 || strcmp(k, "dev") == 0)
        {
          strncpy(c->device_id, v, sizeof(c->device_id) - 1);
          n++;
        }
      else if (strcmp(k, "wifi_if") == 0)
        {
          strncpy(c->wifi_if, v, sizeof(c->wifi_if) - 1);
          n++;
        }
    }

  fclose(f);
  syslog(LOG_INFO, "kitchen: conf loaded %d items (%s:%d dev=%s)\n",
         n, c->mqtt_host, c->mqtt_port, c->device_id);
  return n;
}

/* ---------------- MQTT 下行回调（非 UI 线程） ---------------- */
static void on_ai_text(FAR const char *text, int len)
{
  char tmp[1024];
  FAR const char *out = tmp;

  g_waiting = 0;      /* 收到任何下行都结束等待状态 */

  /* 载荷没有 NUL 结尾，必须先截断拷贝再当字符串用（否则会越界扫描） */
  if (len > (int)sizeof(tmp) - 1)
    {
      len = sizeof(tmp) - 1;
    }

  if (len < 0)
    {
      len = 0;
    }

  memcpy(tmp, text, len);
  tmp[len] = '\0';

  /* 兼容两种下行格式：纯文本，或 {"text":"...","timestamp":..} */
  if (tmp[0] == '{')
    {
      FAR const char *p = strstr(tmp, "\"text\"");
      if (p != NULL)
        {
          p = strchr(p, ':');
          if (p != NULL)
            {
              p = strchr(p, '"');
              if (p != NULL)
                {
                  FAR const char *q = p + 1;
                  char *w = g_ai_buf;
                  int o = 0;

                  while (*q != '\0' && *q != '"' && o < (int)sizeof(g_ai_buf) - 1)
                    {
                      if (*q == '\\' && (q[1] == '"' || q[1] == '\\'))
                        {
                          q++;
                        }

                      w[o++] = *q++;
                    }

                  w[o] = '\0';
                  return;
                }
            }
        }
    }

  strncpy(g_ai_buf, out, sizeof(g_ai_buf) - 1);
  g_ai_buf[sizeof(g_ai_buf) - 1] = '\0';
}

static void on_ai_cmd(FAR const char *json, int len)
{
  /* 指令是给机器看的：普通指令写状态栏，
   * 定时闹钟则给一句显眼的中文提示（不把原始 JSON 甩到屏幕上）。 */
  char tmp[512];

  if (len > (int)sizeof(tmp) - 1)
    {
      len = sizeof(tmp) - 1;
    }

  if (len < 0)
    {
      len = 0;
    }

  memcpy(tmp, json, len);
  tmp[len] = '\0';

  if (strstr(tmp, "\"alarm\"") != NULL)
    {
      FAR const char *p = strstr(tmp, "\"event\"");
      char evt[96] = "";

      if (p != NULL)
        {
          p = strchr(p, ':');
          if (p != NULL)
            {
              p = strchr(p, '"');
              if (p != NULL)
                {
                  FAR const char *q = strchr(p + 1, '"');
                  if (q != NULL && (q - p - 1) < (int)sizeof(evt))
                    {
                      memcpy(evt, p + 1, q - p - 1);
                      evt[q - p - 1] = '\0';
                    }
                }
            }
        }

      snprintf(g_ai_buf, sizeof(g_ai_buf), "提醒时间到：%s", evt);
      return;
    }

  /* 普通指令不直接显示 JSON，只在状态栏给一句提示 */
  snprintf(g_log_buf, sizeof(g_log_buf), "收到云端指令");
}

static void on_ai_audio(FAR const void *data, int len)
{
  snprintf(g_log_buf, sizeof(g_log_buf), "TTS %d bytes playing...", len);
}

/* ---------------- 告警 ---------------- */
void ks_trigger_alert(FAR const char *reason)
{
  int ret;

  g_ks_risk = 3;
  ret = ks_mqtt_publish_alert(g_ks_temp_x10, g_ks_hum_x10, g_ks_risk, reason);
  snprintf(g_alert_buf, sizeof(g_alert_buf),
           ret == 0 ? "Alert sent -> WeChat push" : "MQTT not connected (ret=%d)",
           ret);
}

/* ---------------- WiFi ---------------- */
static void *scan_thread(FAR void *arg)
{
  (void)arg;
  int n;

  ks_ui_log("扫描中 ...");
  n = ks_wifi_scan(g_ap, KS_WIFI_MAX_AP);

  /* 保证 UI 线程看到计数时，g_ap[] 内容已经写完 */
  __sync_synchronize();

  if (n < 0)
    {
      g_ap_count = 0;
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "扫描失败(%d)，检查 WiFi 驱动", n);
    }
  else if (n == 0)
    {
      g_ap_count = 0;
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "没扫到热点，点 Rescan 重试");
    }
  else
    {
      g_ap_count = n;
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "找到 %d 个热点，点选即可", n);
    }

  g_scan_done = 1;      /* 0 个结果也要通知 UI 清掉旧列表 */
  g_scanning = 0;
  return NULL;
}

static void start_scan(void)
{
  pthread_t tid;
  pthread_attr_t attr;

  if (g_scanning)
    {
      return;                  /* 防连点：并发扫描会互相覆盖结果 */
    }

  g_scanning = 1;
  g_ap_count = 0;
  g_scan_done = 0;
  snprintf(g_wifi_buf, sizeof(g_wifi_buf), "扫描中 ...");

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&tid, &attr, scan_thread, NULL) != 0)
    {
      g_scanning = 0;
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "扫描线程创建失败");
    }

  pthread_attr_destroy(&attr);
}

/* 点选热点：填 SSID 并回到 WiFi 页（扫描面板不覆盖输入框） */
static void fill_ssid_cb(lv_event_t *e)
{
  FAR const char *name = (FAR const char *)lv_event_get_user_data(e);

  if (ta_ssid != NULL && name != NULL)
    {
      lv_textarea_set_text(ta_ssid, name);
      snprintf(g_wifi_buf, sizeof(g_wifi_buf),
               "已选 %s，输密码后 Connect", name);
      lv_screen_load(scr_wifi);
    }
}

static void btn_scan_click(lv_event_t *e)
{
  (void)e;
  lv_screen_load(scr_scan);     /* 扫描结果在独立页面显示 */
  start_scan();
}

static void btn_rescan_click(lv_event_t *e)
{
  (void)e;
  start_scan();
}

/* 连接线程参数：字符串在 UI 线程取好，避免跨线程访问 LVGL */
struct ks_conn_arg_s
{
  char ssid[KS_AP_NAME_LEN];
  char psk[64];
};

static void *connect_thread(FAR void *arg)
{
  FAR struct ks_conn_arg_s *a = (FAR struct ks_conn_arg_s *)arg;
  char ip[32] = "?";
  int ret;

  ret = ks_wifi_connect(a->ssid, a->psk);
  if (ret == 0)
    {
      ks_wifi_info(ip, sizeof(ip));
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "已连接 %s  IP %s", a->ssid, ip);
    }
  else
    {
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "连接失败 ret=%d", ret);
    }

  free(a);
  return NULL;
}

static void btn_connect_click(lv_event_t *e)
{
  (void)e;
  FAR const char *ssid;
  FAR const char *psk;
  FAR struct ks_conn_arg_s *a;
  pthread_t tid;
  pthread_attr_t attr;

  hide_kb();                 /* 收起键盘，否则看不到连接结果 */
  if (ta_ssid == NULL || ta_pwd == NULL)
    {
      return;
    }

  ssid = lv_textarea_get_text(ta_ssid);
  psk  = lv_textarea_get_text(ta_pwd);
  if (ssid == NULL || ssid[0] == '\0')
    {
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "请输入 SSID（或点 Scan 选）");
      return;
    }

  a = (FAR struct ks_conn_arg_s *)malloc(sizeof(*a));
  if (a == NULL)
    {
      return;
    }

  memset(a, 0, sizeof(*a));
  strncpy(a->ssid, ssid, sizeof(a->ssid) - 1);
  strncpy(a->psk, psk ? psk : "", sizeof(a->psk) - 1);

  snprintf(g_wifi_buf, sizeof(g_wifi_buf), "正在连接 %s ...", a->ssid);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);   /* 关联 + DHCP 用 16K 栈 */
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&tid, &attr, connect_thread, a) != 0)
    {
      free(a);
      snprintf(g_wifi_buf, sizeof(g_wifi_buf), "连接线程创建失败");
    }

  pthread_attr_destroy(&attr);
}

/* ---------------- AI ---------------- */

/* 离线兜底知识库：大模型问答必须联网（设备->网关->云端），
 * 未连 WiFi 时用本地常见厨房问答作答，避免界面卡在 "Thinking..."。
 */
struct ks_offline_s
{
  FAR const char *key;
  FAR const char *ans;
};

static const struct ks_offline_s g_offline[] =
{
  { "炖鱼",   "中火炖 20-30 分钟，中途看一眼水量别烧干。" },
  { "煮饭",   "米水比 1:1.2，电饭煲标准模式约 30 分钟。" },
  { "蒸蛋",   "蛋水比 1:1.5，中小火蒸 10 分钟，盖保鲜膜更嫩。" },
  { "青菜",   "大火快炒 2-3 分钟，出锅前放盐，颜色更绿。" },
  { "解冻",   "冷藏室解冻最安全；急用可冷水浸泡，每 30 分钟换水。" },
  { "去腥",   "姜葱料酒腌 10 分钟，或冷水下锅焯水去血沫。" },
  { "火候",   "爆炒用大火，炖煮先大火烧开再转中小火。" },
  { "保鲜",   "熟食冷藏不超过 3 天，冷冻不超过 3 个月。" },
  { "清洁",   "油污用热水加小苏打，趁热擦最容易。" },
  { "省电",   "锅底与灶面贴合、加盖烹饪，能省不少电。" },
};

static bool offline_answer(FAR const char *q)
{
  int i;

  for (i = 0; i < (int)(sizeof(g_offline) / sizeof(g_offline[0])); i++)
    {
      if (strstr(q, g_offline[i].key) != NULL)
        {
          snprintf(g_ai_buf, sizeof(g_ai_buf), "[离线]%s", g_offline[i].ans);
          return true;
        }
    }

  return false;
}

static void btn_send_click(lv_event_t *e)
{
  (void)e;
  FAR const char *txt;

  hide_kb();                 /* 收起键盘，否则看不到回答 */
  if (ta_ask == NULL)
    {
      return;
    }

  txt = lv_textarea_get_text(ta_ask);
  if (txt == NULL || txt[0] == '\0')
    {
      snprintf(g_log_buf, sizeof(g_log_buf), "ask: empty");
      return;
    }

  /* 未联网：走本地离线问答，并明确提示需要 WiFi 才能用云端大模型 */
  if (!ks_mqtt_connected())
    {
      if (!offline_answer(txt))
        {
          snprintf(g_ai_buf, sizeof(g_ai_buf),
                   "未联网：请到 WiFi 页连接网络。\n"
                   "云端大模型问答需要上网。");
        }

      snprintf(g_log_buf, sizeof(g_log_buf),
               "OFFLINE (no WiFi) - local answer");
      return;
    }

  int ret = ks_mqtt_publish_ai_text(txt);
  if (ret < 0)
    {
      snprintf(g_ai_buf, sizeof(g_ai_buf), "发送失败 ret=%d\nMQTT 未连接？", ret);
      g_waiting = 0;
      return;
    }

  g_wait_ticks = 0;
  g_waiting = 1;
  snprintf(g_ai_buf, sizeof(g_ai_buf), "Thinking...");
}

static void btn_rec_click(lv_event_t *e)
{
  (void)e;

  hide_kb();
  if (!ks_mqtt_connected())
    {
      snprintf(g_ai_buf, sizeof(g_ai_buf),
               "未联网：语音问答需要 WiFi。\n请先到 WiFi 页连接网络。");
      snprintf(g_log_buf, sizeof(g_log_buf), "OFFLINE (no WiFi) - record skipped");
      return;
    }

  g_wait_ticks = 0;
  g_waiting = 1;
  snprintf(g_ai_buf, sizeof(g_ai_buf), "Recording %ds ... 请说话", KS_REC_SECONDS);
  ks_audio_record_and_upload(KS_REC_SECONDS);
}

/* 把当前 AI 回答 / 提示转发到用户微信（Server酱通道） */
static void btn_wechat_click(lv_event_t *e)
{
  (void)e;

  hide_kb();
  if (g_ai_buf[0] == '\0')
    {
      snprintf(g_log_buf, sizeof(g_log_buf), "nothing to send");
      return;
    }

  if (!ks_mqtt_connected())
    {
      snprintf(g_log_buf, sizeof(g_log_buf), "未联网，微信发送失败（需 WiFi）");
      return;
    }

  if (ks_mqtt_publish_wechat(g_ai_buf) < 0)
    {
      snprintf(g_log_buf, sizeof(g_log_buf), "WeChat send failed");
      return;
    }

  snprintf(g_log_buf, sizeof(g_log_buf), "sent to WeChat OK");
}

/* ---------------- 页面构建 ---------------- */
static void to_home_cb(lv_event_t *e)
{
  (void)e;
  hide_kb();
  lv_screen_load(scr_home);
}

static void to_wifi_cb(lv_event_t *e)
{
  (void)e;
  hide_kb();
  lv_screen_load(scr_wifi);
}

static void to_ai_cb(lv_event_t *e)
{
  (void)e;
  hide_kb();
  lv_screen_load(scr_ai);
}

static void to_alert_cb(lv_event_t *e)
{
  (void)e;
  hide_kb();
  lv_screen_load(scr_alert);
}

static lv_obj_t *make_back(FAR lv_obj_t *scr, lv_event_cb_t cb)
{
  lv_obj_t *b = lv_button_create(scr);
  lv_obj_set_size(b, 68, 34);
  lv_obj_align(b, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_label_set_text(lv_label_create(b), "< Back");
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

/* 所有会显示中文的标签都必须显式套用中文字体，
 * 否则默认的 montserrat 没有 CJK 字形，中文会整句不显示。 */
static void use_cn_font(FAR lv_obj_t *lbl)
{
  if (lbl != NULL && g_font_cn != NULL)
    {
      lv_obj_set_style_text_font(lbl, g_font_cn, 0);
    }
}

/* 收起软键盘（否则键盘会一直盖住下半屏） */
static void hide_kb(void)
{
  if (kb_wifi != NULL)
    {
      lv_obj_add_flag(kb_wifi, LV_OBJ_FLAG_HIDDEN);
    }

  if (kb_ai != NULL)
    {
      lv_obj_add_flag(kb_ai, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_title(lv_obj_t *scr, FAR const char *txt)
{
  lv_obj_t *t = lv_label_create(scr);
  lv_label_set_text(t, txt);
  if (g_font_cn)
    {
      lv_obj_set_style_text_font(t, g_font_cn, 0);
    }

  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
  return t;
}

static void ta_focus_cb(lv_event_t *e)
{
  lv_obj_t *ta = lv_event_get_target(e);
  lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
  if (ta != NULL && kb != NULL)
    {
      lv_keyboard_set_textarea(kb, ta);
      lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(kb);
    }
}

static void kb_done_cb(lv_event_t *e)
{
  lv_obj_t *kb = lv_event_get_target(e);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* 统一按钮创建：显式坐标，全部相对屏幕尺寸算，不会互相压盖 */
static lv_obj_t *mk_btn(FAR lv_obj_t *parent, FAR const char *txt,
                        int x, int y, int w, int h,
                        lv_event_cb_t cb)
{
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_label_set_text(lv_label_create(b), txt);
  if (cb != NULL)
    {
      lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    }

  return b;
}

static void build_home(void)
{
  int bw = (g_scr_w >= 300) ? 124 : 104;
  int bh = 42;
  int sx = bw + 16;

  scr_home = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_home, lv_color_hex(0x101418), 0);

  lv_obj_t *t = lv_label_create(scr_home);
  lv_label_set_text(t, "Smart Kitchen v2");
  lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

  /* 左侧三个功能入口 */
  mk_btn(scr_home, "WiFi",         8, 30,  bw, bh, to_wifi_cb);
  mk_btn(scr_home, "AI Voice",     8, 82,  bw, bh, to_ai_cb);
  mk_btn(scr_home, "WeChat Alert", 8, 134, bw, bh, to_alert_cb);

  /* 右侧状态区 */
  lbl_home_status = lv_label_create(scr_home);
  lv_label_set_text(lbl_home_status, "booting...");
  lv_obj_set_style_text_color(lbl_home_status, lv_color_hex(0x9AD7FF), 0);
  use_cn_font(lbl_home_status);
  lv_obj_set_pos(lbl_home_status, sx, 30);
  lv_obj_set_width(lbl_home_status, g_scr_w - sx - 8);
  lv_label_set_long_mode(lbl_home_status, LV_LABEL_LONG_WRAP);
}

static void build_wifi(void)
{
  int fw = g_scr_w - 16;         /* 输入框宽度 */

  scr_wifi = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_wifi, lv_color_hex(0x101418), 0);
  make_back(scr_wifi, to_home_cb);
  make_title(scr_wifi, "WiFi Setup");

  ta_ssid = lv_textarea_create(scr_wifi);
  lv_textarea_set_one_line(ta_ssid, true);
  lv_textarea_set_placeholder_text(ta_ssid, "SSID");
  lv_obj_set_pos(ta_ssid, 8, 40);
  lv_obj_set_size(ta_ssid, fw, 30);

  ta_pwd = lv_textarea_create(scr_wifi);
  lv_textarea_set_one_line(ta_pwd, true);
  lv_textarea_set_placeholder_text(ta_pwd, "password");
  lv_textarea_set_password_mode(ta_pwd, true);   /* 屏上不明文显示密码 */
  lv_obj_set_pos(ta_pwd, 8, 74);
  lv_obj_set_size(ta_pwd, fw, 30);

  /* 扫描结果放到独立页面，这里只留按钮，绝不遮住输入框；
   * 按钮放在 y=108，键盘(底部 100px)弹起时也不会盖住按钮 */
  mk_btn(scr_wifi, "Scan",    8,            108, 96, 32, btn_scan_click);
  mk_btn(scr_wifi, "Connect", g_scr_w - 104, 108, 96, 32, btn_connect_click);

  lbl_wifi_status = lv_label_create(scr_wifi);
  lv_label_set_text(lbl_wifi_status, "先 Scan 选热点，或直接输入");
  lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0xFFE08A), 0);
  use_cn_font(lbl_wifi_status);
  lv_obj_set_pos(lbl_wifi_status, 8, 146);
  lv_obj_set_width(lbl_wifi_status, fw);
  lv_label_set_long_mode(lbl_wifi_status, LV_LABEL_LONG_WRAP);

  kb_wifi = lv_keyboard_create(scr_wifi);
  lv_obj_set_size(kb_wifi, g_scr_w, (g_scr_h / 2 > 100) ? 100 : g_scr_h / 2);
  lv_obj_align(kb_wifi, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb_wifi, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(kb_wifi, ta_ssid);

  lv_obj_add_event_cb(ta_ssid, ta_focus_cb, LV_EVENT_CLICKED, kb_wifi);
  lv_obj_add_event_cb(ta_pwd, ta_focus_cb, LV_EVENT_CLICKED, kb_wifi);
  lv_obj_add_event_cb(kb_wifi, kb_done_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb_wifi, kb_done_cb, LV_EVENT_CANCEL, NULL);
}

/* 扫描结果独立页面：列表占满，不再压住 WiFi 页的输入框 */
static void build_scan(void)
{
  scr_scan = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_scan, lv_color_hex(0x101418), 0);
  make_back(scr_scan, to_wifi_cb);   /* 扫描页返回 WiFi 页，与点选后一致 */
  make_title(scr_scan, "Select WiFi");

  mk_btn(scr_scan, "Rescan", g_scr_w - 76, 4, 72, 30, btn_rescan_click);

  list_scan = lv_list_create(scr_scan);
  lv_obj_set_pos(list_scan, 4, 38);
  lv_obj_set_size(list_scan, g_scr_w - 8, g_scr_h - 64);

  lbl_scan_status = lv_label_create(scr_scan);
  lv_label_set_text(lbl_scan_status, "scanning...");
  lv_obj_set_style_text_color(lbl_scan_status, lv_color_hex(0xFFE08A), 0);
  use_cn_font(lbl_scan_status);
  lv_obj_align(lbl_scan_status, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void build_ai(void)
{
  int fw = g_scr_w - 16;
  int bw = (g_scr_w - 32) / 3;      /* 三个按钮一行 */

  scr_ai = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_ai, lv_color_hex(0x101418), 0);
  make_back(scr_ai, to_home_cb);
  make_title(scr_ai, "AI Assistant");

  ta_ask = lv_textarea_create(scr_ai);
  lv_textarea_set_one_line(ta_ask, true);
  lv_textarea_set_placeholder_text(ta_ask, "ask something...");
  lv_obj_set_pos(ta_ask, 8, 40);
  lv_obj_set_size(ta_ask, fw, 30);

  mk_btn(scr_ai, "Send",    8,             74, bw, 32, btn_send_click);
  mk_btn(scr_ai, "Rec 8s",  8 + bw + 8,    74, bw, 32, btn_rec_click);
  mk_btn(scr_ai, "To WeChat", 8 + 2 * (bw + 8), 74, bw, 32, btn_wechat_click);

  /* 回答放进可滚动容器：长中文回答不会被裁掉，也不会盖住底部状态行 */
  lv_obj_t *ans_box = lv_obj_create(scr_ai);
  lv_obj_set_pos(ans_box, 8, 112);
  lv_obj_set_size(ans_box, fw, g_scr_h - 112 - 26);
  lv_obj_set_style_bg_opa(ans_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ans_box, 0, 0);
  lv_obj_set_style_pad_all(ans_box, 0, 0);
  lv_obj_add_flag(ans_box, LV_OBJ_FLAG_SCROLLABLE);

  lbl_ai_text = lv_label_create(ans_box);
  lv_label_set_text(lbl_ai_text, "answer shows here");
  use_cn_font(lbl_ai_text);
  lv_obj_set_style_text_color(lbl_ai_text, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_width(lbl_ai_text, fw - 8);
  lv_label_set_long_mode(lbl_ai_text, LV_LABEL_LONG_WRAP);

  lbl_ai_status = lv_label_create(scr_ai);
  lv_label_set_text(lbl_ai_status, "");
  lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xFFE08A), 0);
  use_cn_font(lbl_ai_status);
  lv_obj_align(lbl_ai_status, LV_ALIGN_BOTTOM_MID, 0, -2);

  kb_ai = lv_keyboard_create(scr_ai);
  lv_obj_set_size(kb_ai, g_scr_w, (g_scr_h / 2 > 100) ? 100 : g_scr_h / 2);
  lv_obj_align(kb_ai, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb_ai, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(kb_ai, ta_ask);
  lv_obj_add_event_cb(ta_ask, ta_focus_cb, LV_EVENT_CLICKED, kb_ai);
  lv_obj_add_event_cb(kb_ai, kb_done_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb_ai, kb_done_cb, LV_EVENT_CANCEL, NULL);
}

static void btn_alert_click(lv_event_t *e)
{
  (void)e;
  hide_kb();
  snprintf(g_alert_buf, sizeof(g_alert_buf), "sending alert...");
  ks_trigger_alert("manual_test");
}

static void build_alert(void)
{
  scr_alert = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_alert, lv_color_hex(0x101418), 0);
  make_back(scr_alert, to_home_cb);
  make_title(scr_alert, "WeChat Alert");

  lbl_alert_state = lv_label_create(scr_alert);
  lv_label_set_text(lbl_alert_state, "T:95.0C  H:60%  Risk:OK");
  lv_obj_set_style_text_color(lbl_alert_state, lv_color_hex(0x9AD7FF), 0);
  lv_obj_set_pos(lbl_alert_state, 8, 40);
  lv_obj_set_width(lbl_alert_state, g_scr_w - 16);
  lv_label_set_long_mode(lbl_alert_state, LV_LABEL_LONG_WRAP);

  mk_btn(scr_alert, "Trigger Alert", (g_scr_w - 170) / 2, 76, 170, 46,
         btn_alert_click);

  lv_obj_t *l = lv_label_create(scr_alert);
  lv_label_set_text(l, "");
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFE08A), 0);
  lv_obj_set_pos(l, 8, 132);
  lv_obj_set_width(l, g_scr_w - 16);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_user_data(scr_alert, l);        /* 复用：存状态标签指针 */
}

/* ---------------- UI 定时刷新（唯一允许碰 LVGL 的地方） ---------------- */
static void ui_timer(lv_timer_t *t)
{
  (void)t;
  static int tick = 0;
  lv_obj_t *cur = lv_screen_active();

  tick++;
  if ((tick % 20) == 0)
    {
      g_ks_temp_x10 += 5;
      if (g_ks_temp_x10 > 1200)
        {
          g_ks_temp_x10 = 900;
        }

      g_ks_risk = (g_ks_temp_x10 > 1100) ? 2 : 0;
    }

  if (cur == scr_home && lbl_home_status != NULL)
    {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "T:%d.%dC H:%d%% Risk:%s\nMQTT:%s\n%.70s",
               g_ks_temp_x10 / 10, g_ks_temp_x10 % 10, g_ks_hum_x10 / 10,
               g_ks_risk >= 2 ? "HIGH" : "OK",
               ks_mqtt_connected() ? "connected" : "offline",
               g_log_buf);
      lv_label_set_text(lbl_home_status, buf);
    }
  else if (cur == scr_wifi && lbl_wifi_status != NULL)
    {
      char buf[320];
      snprintf(buf, sizeof(buf), "%.120s\n%.60s", g_wifi_buf, g_log_buf);
      lv_label_set_text(lbl_wifi_status, buf);
    }
  else if (cur == scr_scan)
    {
      /* 一次扫描结束后刷新列表：0 个结果也会先清掉旧项 */
      if (g_scan_done && list_scan != NULL)
        {
          lv_obj_clean(list_scan);
          for (int i = 0; i < g_ap_count; i++)
            {
              lv_obj_t *b = lv_list_add_button(list_scan, NULL, g_ap[i]);
              lv_obj_add_event_cb(b, fill_ssid_cb, LV_EVENT_CLICKED, g_ap[i]);
            }

          g_scan_done = 0;
        }

      if (lbl_scan_status != NULL)
        {
          lv_label_set_text(lbl_scan_status, g_wifi_buf);
        }
    }
  else if (cur == scr_ai)
    {
      char st[160];

      if (lbl_ai_text != NULL && g_ai_buf[0] != '\0')
        {
          lv_label_set_text(lbl_ai_text, g_ai_buf);
          lv_obj_scroll_to_view(lbl_ai_text, LV_ANIM_OFF);
        }

      if (g_waiting)
        {
          int el;

          g_wait_ticks++;                 /* 本定时器 500ms 一次 */
          el = g_wait_ticks / 2;

          if (el > 20)
            {
              snprintf(st, sizeof(st), "云端无响应 %ds：检查网关是否运行/网络", el);
            }
          else
            {
              snprintf(st, sizeof(st), "等待云端回答 %ds ...", el);
            }
        }
      else
        {
          snprintf(st, sizeof(st), "%.60s", g_log_buf);
        }

      if (lbl_ai_status != NULL)
        {
          lv_label_set_text(lbl_ai_status, st);
        }
    }
  else if (cur == scr_alert)
    {
      lv_obj_t *l = (lv_obj_t *)lv_obj_get_user_data(scr_alert);
      char buf[128];
      snprintf(buf, sizeof(buf), "T:%d.%dC H:%d%% Risk:%s",
               g_ks_temp_x10 / 10, g_ks_temp_x10 % 10, g_ks_hum_x10 / 10,
               g_ks_risk >= 2 ? "HIGH" : "OK");
      if (lbl_alert_state != NULL)
        {
          lv_label_set_text(lbl_alert_state, buf);
        }

      if (l != NULL)
        {
          lv_label_set_text(l, g_alert_buf);
        }
    }
}

/* ---------------- 字体 ---------------- */
static void init_font(void)
{
#if LV_USE_FREETYPE
  g_font_cn = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                      16, LV_FREETYPE_FONT_STYLE_NORMAL);
  if (g_font_cn == NULL)
    {
      syslog(LOG_WARNING, "kitchen: no CN font, fallback\n");
    }
#endif
}

/* ---------------- 串口兜底命令 ---------------- */
static int cli_mode(int argc, FAR char *argv[])
{
  FAR const char *cmd = argv[1];

  ks_cfg_load(&g_cfg);
  ks_mqtt_set_broker(g_cfg.mqtt_host, g_cfg.mqtt_port, g_cfg.device_id);

  if (strcmp(cmd, "ask") == 0 && argc > 2)
    {
      ks_mqtt_start();
      sleep(3);
      int ret = ks_mqtt_publish_ai_text(argv[2]);
      printf("ask sent ret=%d\n", ret);
      sleep(8);
      return 0;
    }

  if (strcmp(cmd, "rec") == 0)
    {
      int secs = (argc > 2) ? atoi(argv[2]) : 3;

      /* /tmp 是内存盘：16k/16bit 单声道约 32KB/s，必须夹住时长免得 OOM */
      if (secs < 1)
        {
          secs = 1;
        }

      if (secs > 20)
        {
          secs = 20;
        }
      ks_mqtt_start();
      sleep(3);
      if (ks_audio_record(KS_REC_PCM, secs) == 0)
        {
          FILE *f = fopen(KS_REC_PCM, "rb");
          if (f != NULL)
            {
              long sz;
              fseek(f, 0, SEEK_END);
              sz = ftell(f);
              fseek(f, 0, SEEK_SET);
              if (sz > 0 && sz < (2 * 1024 * 1024))
                {
                  char *b = malloc(sz);
                  if (b != NULL && fread(b, 1, sz, f) == (size_t)sz)
                    {
                      int r = ks_mqtt_publish_ai_pcm(b, (int)sz);
                      printf("rec uploaded %ld bytes ret=%d\n", sz, r);
                    }

                  free(b);
                }

              fclose(f);
            }
        }

      sleep(10);
      return 0;
    }

  if (strcmp(cmd, "alert") == 0)
    {
      ks_mqtt_start();
      sleep(3);
      ks_trigger_alert("cli_test");
      printf("alert ret buf=%s\n", g_alert_buf);
      sleep(2);
      return 0;
    }

  if (strcmp(cmd, "status") == 0)
    {
      char ip[64] = "?";
      ks_wifi_info(ip, sizeof(ip));
      printf("mqtt=%s host=%s dev=%s ip=%s\n",
             ks_mqtt_connected() ? "up" : "down",
             g_cfg.mqtt_host, g_cfg.device_id, ip);
      return 0;
    }

  if (strcmp(cmd, "wifi") == 0 && argc > 3)
    {
      int ret = ks_wifi_connect(argv[2], argv[3]);
      printf("wifi connect ret=%d\n", ret);
      return 0;
    }

  printf("usage: kitchen_smart [ask \"q\" | rec N | alert | status | wifi SSID PWD]\n");
  return 0;
}

/* ---------------- main ---------------- */
int kitchen_smart_main(int argc, FAR char *argv[])
{
  if (argc > 1)
    {
      return cli_mode(argc, argv);
    }

  if (lv_is_initialized())
    {
      syslog(LOG_ERR, "kitchen: lvgl already initialized\n");
      return -1;
    }

  /* 配置 + MQTT */
  ks_cfg_load(&g_cfg);
  ks_mqtt_set_broker(g_cfg.mqtt_host, g_cfg.mqtt_port, g_cfg.device_id);
  ks_mqtt_set_handlers(on_ai_text, on_ai_cmd, on_ai_audio);

  /* LVGL */
  lv_init();
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  lv_nuttx_dsc_init(&info);
#ifdef CONFIG_LV_USE_NUTTX_LCD
  info.fb_path = "/dev/lcd0";
#endif
#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  info.input_path = CONFIG_EXAMPLES_LVGLDEMO_INPUT_DEVPATH;
#endif
  lv_nuttx_init(&info, &result);
  usleep(100 * 1000);

  if (result.disp == NULL)
    {
      syslog(LOG_ERR, "kitchen: lvgl init failed\n");
      return 1;
    }

  init_font();

  /* 先量屏幕尺寸，再按尺寸建页面（本板是横屏 320x240） */
  scr_probe();

  /* 建页面 */
  build_home();
  build_wifi();
  build_scan();
  build_ai();
  build_alert();
  lv_screen_load(scr_home);

  lv_timer_create(ui_timer, 500, NULL);

  /* 后台 MQTT（自动重连） */
  ks_mqtt_start();

  syslog(LOG_INFO, "kitchen_smart v2 started\n");

  while (1)
    {
      uint32_t idle = lv_timer_handler();

      /* LVGL 无就绪定时器时返回 LV_NO_TIMER_READY(0xFFFFFFFF)，
       * 直接乘 1000 会溢出成 71 分钟休眠（界面假死），必须夹紧 */
      if (idle > 30)
        {
          idle = 30;
        }

      usleep(idle ? idle * 1000 : 5000);
    }

  return 0;
}
