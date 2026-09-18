/****************************************************************************
 * kitchen_smart.h - openvela 智能厨房 v2（Gemini-S1 / 2.8" SPI）
 *
 * v2 变化：
 *  - UI 每页独立 screen + 返回键（WiFi/AI/告警页可自由进出）
 *  - 软键盘输入、扫描结果点选、中文字体
 *  - 录音/播放放独立线程（不阻塞 LVGL）
 *  - MQTT 参数读 /data/etc/kitchen.conf，断线自动重连
 *  - 串口兜底命令：kitchen_smart ask/rec/alert/status/wifi
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_KITCHEN_SMART_H
#define __APPS_VENDOR_ALLWINNERTECH_KITCHEN_SMART_H

#include <stdbool.h>
#include <stdint.h>

/* 编译期默认值（可被 /data/etc/kitchen.conf 覆盖） */
#ifndef CONFIG_KITCHEN_SMART_MQTT_HOST
#  define CONFIG_KITCHEN_SMART_MQTT_HOST "broker.emqx.io"
#endif
#ifndef CONFIG_KITCHEN_SMART_MQTT_PORT
#  define CONFIG_KITCHEN_SMART_MQTT_PORT 1883
#endif
#ifndef CONFIG_KITCHEN_SMART_DEVICE_ID
#  define CONFIG_KITCHEN_SMART_DEVICE_ID "device001"
#endif
#ifndef CONFIG_KITCHEN_SMART_WIFI_IFNAME
#  define CONFIG_KITCHEN_SMART_WIFI_IFNAME "wlan0"
#endif

#define KS_TOPIC_REQ       "openvela/kitchen/%s/ai/request"
#define KS_TOPIC_RESP_TEXT "openvela/kitchen/%s/ai/response/text"
#define KS_TOPIC_RESP_AUD  "openvela/kitchen/%s/ai/response/audio"
#define KS_TOPIC_CMD       "openvela/kitchen/%s/cmd"
#define KS_TOPIC_STATE     "openvela/kitchen/%s/state"
#define KS_TOPIC_ALERT     "openvela/kitchen/%s/alert"

#define KS_WIFI_CONF_PATH  "/data/etc/wifi/wapi.conf"
#define KS_APP_CONF_PATH   "/data/etc/kitchen.conf"
#define KS_REC_PCM         "/tmp/kitchen_rec.pcm"
#define KS_TTS_WAV         "/tmp/kitchen_tts.wav"
#define KS_TTS_WAV_A       "/tmp/kitchen_tts_a.wav"
#define KS_TTS_WAV_B       "/tmp/kitchen_tts_b.wav"
#define KS_TTS_PCM         "/tmp/kitchen_tts.pcm"
#define KS_TTS_PCM2        "/tmp/kitchen_tts_st.pcm"
#define KS_WIFI_MAX_AP     16
#define KS_AP_NAME_LEN     40
#define KS_REC_SECONDS     8      /* 录音时长（秒）：3 秒太短，改成 8 秒 */

/* ---------------- 运行配置 ---------------- */
struct ks_cfg_s
{
  char mqtt_host[64];
  int  mqtt_port;
  char device_id[32];
  char wifi_if[16];
};
void ks_cfg_defaults(FAR struct ks_cfg_s *c);
int  ks_cfg_load(FAR struct ks_cfg_s *c);   /* 读 /data/etc/kitchen.conf */

/* ---------------- WiFi ---------------- */
int  ks_wifi_ifup(void);
int  ks_wifi_scan(FAR char aps[][KS_AP_NAME_LEN], int max);  /* 返回数量 */
int  ks_wifi_connect(FAR const char *ssid, FAR const char *psk);
int  ks_wifi_info(FAR char *buf, int buflen);

/* ---------------- MQTT ---------------- */
typedef void (*ks_on_text_t)(FAR const char *text, int len);
typedef void (*ks_on_cmd_t)(FAR const char *json, int len);
typedef void (*ks_on_audio_t)(FAR const void *data, int len);

void ks_mqtt_set_broker(FAR const char *host, int port, FAR const char *dev);
void ks_mqtt_set_handlers(ks_on_text_t t, ks_on_cmd_t c, ks_on_audio_t a);
int  ks_mqtt_start(void);          /* 启动带自动重连的后台线程 */
void ks_mqtt_stop(void);
bool ks_mqtt_connected(void);
int  ks_mqtt_publish(FAR const char *topic, FAR const void *payload,
                     int len, int qos);
int  ks_mqtt_publish_ai_text(FAR const char *text);
int  ks_mqtt_publish_wechat(FAR const char *text);   /* 转发文本到手机微信 */
int  ks_mqtt_publish_ai_pcm(FAR const void *pcm, int len);
int  ks_mqtt_publish_alert(int temp_x10, int hum_x10, int risk,
                           FAR const char *reason);

/* ---------------- 音频 ---------------- */
int  ks_audio_record(FAR const char *path, int seconds);  /* 16k mono PCM */
int  ks_audio_play(FAR const char *path);                 /* wav/pcm 播放 */
int  ks_audio_record_and_upload(int seconds);             /* 录音->MQTT 上传 */

/* ---------------- 应用状态（供 UI 展示） ---------------- */
extern volatile int  g_ks_risk;      /* 0 正常 1 注意 2 警告 3 紧急 */
extern volatile int  g_ks_temp_x10;
extern volatile int  g_ks_hum_x10;

void ks_trigger_alert(FAR const char *reason);   /* 产生告警并上报 */

/* 日志（会显示到当前页面状态栏） */
void ks_ui_log(FAR const char *fmt, ...);

#endif
