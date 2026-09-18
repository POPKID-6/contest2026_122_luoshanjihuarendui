/****************************************************************************
 * kitchen_wifi.c - WiFi 扫描/连接（v2.1 修复版）
 *
 * 本次修复的 5 个问题（原版扫不到热点 / 卡在扫描 / 连不上加密热点）：
 *  1) 扫描前没有 ifup：wlan0 处于 down 状态时扫不到任何 AP
 *  2) wapi_scan_stat() 的语义是 0=数据就绪 / 1=未就绪 / <0=出错，
 *     原代码写成 `while (ret == 0)` —— 一旦扫描完成就永远循环（卡死）
 *  3) 扫描类型传了 1，实际 IW_SCAN_TYPE_PASSIVE=1（被动监听），
 *     很多路由器扫不到；改为 IW_SCAN_TYPE_ACTIVE=0
 *  4) 连接只调了 wapi_set_essid()，从来没下发 WPA2 口令，
 *     加密热点必然连不上；改为走 wpa_driver_wext_associate() 完整流程
 *  5) 关联成功后没有取 IP；补上 DHCP（dhcpc 库）
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>

#ifdef CONFIG_WIRELESS_WAPI
#  include <wireless/wapi.h>
#  include <net/if.h>
#endif

#ifdef CONFIG_NETUTILS_DHCPC
#  include <netutils/dhcpc.h>
#endif
#ifdef CONFIG_NETUTILS_NETLIB
#  include <netutils/netlib.h>
#endif

#include "kitchen_smart.h"

#define KS_IF CONFIG_KITCHEN_SMART_WIFI_IFNAME

/* 扫描等待：200ms × 40 = 最长 8 秒，绝不无限等待 */
#define KS_SCAN_POLL_MS   200
#define KS_SCAN_POLL_MAX  40

int ks_wifi_ifup(void)
{
#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  int ret;
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_set_ifup(sock, KS_IF);
  close(sock);
  return ret;
#else
  return -ENOSYS;
#endif
}

int ks_wifi_scan(FAR char aps[][KS_AP_NAME_LEN], int max)
{
#ifdef CONFIG_WIRELESS_WAPI
  int sock, ret, n = 0, tries;
  struct wapi_list_s list;

  /* 关键：先拉起接口，否则扫不到任何 AP */
  ks_wifi_ifup();
  usleep(200 * 1000);

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  /* 主动扫描优先，失败退回普通扫描 */
  ret = wapi_escan_init(sock, KS_IF, IW_SCAN_TYPE_ACTIVE, NULL);
  if (ret < 0)
    {
      ret = wapi_scan_init(sock, KS_IF, NULL);
    }

  if (ret < 0)
    {
      ks_ui_log("扫描启动失败 ret=%d", ret);
      close(sock);
      return ret;
    }

  /* 等待扫描完成：0=就绪 1=未就绪 <0=错误 */
  tries = 0;
  do
    {
      usleep(KS_SCAN_POLL_MS * 1000);
      ret = wapi_scan_stat(sock, KS_IF);
    }
  while (ret == 1 && ++tries < KS_SCAN_POLL_MAX);

  if (ret < 0)
    {
      ks_ui_log("扫描出错 ret=%d", ret);
      close(sock);
      return ret;
    }

  memset(&list, 0, sizeof(list));
  ret = wapi_scan_coll(sock, KS_IF, &list);
  close(sock);
  if (ret < 0)
    {
      /* 失败时链表里可能已经挂了若干已分配节点，必须释放 */
      wapi_scan_coll_free(&list);
      ks_ui_log("读取扫描结果失败 ret=%d", ret);
      return ret;
    }

  for (struct wapi_scan_info_s *ap = list.head.scan;
       ap != NULL && n < max; ap = ap->next)
    {
      if (ap->essid[0] != '\0')
        {
          strncpy(aps[n], ap->essid, KS_AP_NAME_LEN - 1);
          aps[n][KS_AP_NAME_LEN - 1] = '\0';
          n++;
        }
    }

  wapi_scan_coll_free(&list);
  return n;
#else
  (void)aps; (void)max;
  return -ENOSYS;
#endif
}

#ifdef CONFIG_NETUTILS_DHCPC
static int ks_wifi_get_mac(FAR uint8_t mac[6])
{
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct ifreq ifr;

  if (s < 0)
    {
      return -1;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, KS_IF, IFNAMSIZ - 1);
  if (ioctl(s, SIOCGIFHWADDR, (unsigned long)&ifr) < 0)
    {
      close(s);
      return -1;
    }

  memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
  close(s);
  return 0;
}
#endif

/* 取 IP（DHCP），成功把点分十进制写进 ipbuf */
static int ks_wifi_dhcp(FAR char *ipbuf, int iplen)
{
#ifdef CONFIG_NETUTILS_DHCPC
  struct dhcpc_state ds;
  uint8_t mac[6];
  FAR void *h;
  int ret;

  if (ks_wifi_get_mac(mac) < 0)
    {
      return -1;
    }

  h = dhcpc_open(KS_IF, mac, 6);
  if (h == NULL)
    {
      return -1;
    }

  memset(&ds, 0, sizeof(ds));
  ret = dhcpc_request(h, &ds);
  dhcpc_close(h);
  if (ret != 0)
    {
      return -1;
    }

  /* 关键：dhcpc 库只应用了 IP，掩码/网关/DNS 必须自己设，
   * 否则没有 DNS 就解析不了 broker 域名，MQTT 永远连不上。 */
#ifdef CONFIG_NETUTILS_NETLIB
  netlib_set_ipv4addr(KS_IF, &ds.ipaddr);
  netlib_set_ipv4netmask(KS_IF, &ds.netmask);
  netlib_set_dripv4addr(KS_IF, &ds.default_router);

  /* 路由器没下发 DNS 时兜底成公共 DNS，否则域名解析不了 */
  struct in_addr dns = ds.dnsaddr;
  if (dns.s_addr == 0)
    {
      inet_aton("223.5.5.5", &dns);
    }

  netlib_set_ipv4dnsaddr(&dns);
#endif

  if (ipbuf != NULL && iplen > 0)
    {
      snprintf(ipbuf, iplen, "%s", inet_ntoa(ds.ipaddr));
    }

  return 0;
#else
  (void)ipbuf; (void)iplen;
  return -1;
#endif
}

/* 阻塞式连接：请在独立线程里调用（UI 线程不能阻塞） */
int ks_wifi_connect(FAR const char *ssid, FAR const char *psk)
{
  char buf[512];
  int fd;

  if (ssid == NULL || ssid[0] == '\0')
    {
      return -EINVAL;
    }

  /* 1) 保存配置：必须是厂商 wapi 的「嵌套 JSON」格式，
   *    例如 {"wlan0":{"mode":2,"auth":4,"cmode":8,"alg":3,"ssid":..,"psk":..}}，
   *    这样 wapi reconnect / 开机自动重连才读得到（扁平格式读不了） */
  bool sec = (psk != NULL && strlen(psk) >= 8);

  /* 写入前做最小化 JSON 转义，避免密码里的 " 或 \ 破坏配置文件 */
  char ssid_js[KS_AP_NAME_LEN * 2];
  char psk_js[128];
  int si, so;

  for (si = 0, so = 0; ssid[si] != '\0' && so < (int)sizeof(ssid_js) - 2; si++)
    {
      if (ssid[si] == '"' || ssid[si] == '\\')
        {
          ssid_js[so++] = '\\';
        }

      ssid_js[so++] = ssid[si];
    }

  ssid_js[so] = '\0';

  for (si = 0, so = 0;
       psk != NULL && psk[si] != '\0' && so < (int)sizeof(psk_js) - 2;
       si++)
    {
      if (psk[si] == '"' || psk[si] == '\\')
        {
          psk_js[so++] = '\\';
        }

      psk_js[so++] = psk[si];
    }

  psk_js[so] = '\0';

  snprintf(buf, sizeof(buf),
           "{\n"
           "    \"%s\": {\n"
           "        \"mode\": 2,\n"
           "        \"auth\": %d,\n"
           "        \"cmode\": %d,\n"
           "        \"alg\": %d,\n"
           "        \"ssid\": \"%s\",\n"
           "        \"bssid\": \"\",\n"
           "        \"psk\": \"%s\"\n"
           "    }\n"
           "}\n",
           KS_IF,
           sec ? 4 : 1,     /* auth : 4=WPA2, 1=开放 */
           sec ? 8 : 1,     /* cmode: 8=CCMP, 1=NONE */
           sec ? 3 : 0,     /* alg  : 3=CCMP, 0=NONE */
           ssid_js, psk_js);

  mkdir("/data", 0777);
  mkdir("/data/etc", 0777);
  mkdir("/data/etc/wifi", 0777);

  fd = open(KS_WIFI_CONF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0)
    {
      write(fd, buf, strlen(buf));
      close(fd);
    }

#ifdef CONFIG_WIRELESS_WAPI
  struct wpa_wconfig_s conf;
  char ip[24] = "?";
  int sock, ret, i;

  ks_ui_log("连接 %s ...", ssid);

  sock = wapi_make_socket();
  if (sock < 0)
    {
      ks_ui_log("wifi socket 失败 %d", sock);
      return sock;
    }

  wapi_set_ifup(sock, KS_IF);                 /* 4) 先拉起接口 */
  wpa_driver_wext_disconnect(sock, KS_IF);    /* 断开旧连接 */
  usleep(300 * 1000);

  memset(&conf, 0, sizeof(conf));
  conf.sta_mode = WAPI_MODE_MANAGED;
  conf.ifname   = KS_IF;
  conf.ssid     = ssid;
  conf.ssidlen  = strlen(ssid);
  if (sec)
    {
      /* 5) 关键修复：WPA2-PSK + CCMP 口令必须下发 */
      conf.auth_wpa    = IW_AUTH_WPA_VERSION_WPA2;
      conf.cipher_mode = IW_AUTH_CIPHER_CCMP;
      conf.alg         = WPA_ALG_CCMP;
      conf.passphrase  = psk;
      conf.phraselen   = strlen(psk);
    }
  else
    {
      /* 开放热点 */
      conf.auth_wpa    = IW_AUTH_WPA_VERSION_DISABLED;
      conf.cipher_mode = IW_AUTH_CIPHER_NONE;
      conf.alg         = WPA_ALG_NONE;
    }

  /* 一步完成: mode -> auth 版本 -> 加密套件 -> 口令 -> SSID -> 关联 */
  ret = wpa_driver_wext_associate(&conf);
  if (ret >= 0)
    {
      wapi_set_power_save(sock, KS_IF, false);   /* 关省电，连接更稳 */
    }

  close(sock);

  if (ret < 0)
    {
      ks_ui_log("关联失败 ret=%d（密码错/信号弱？）", ret);
      return ret;
    }

  ks_ui_log("关联成功，正在获取 IP ...");

  /* 6) 拿 IP */
  for (i = 0; i < 3; i++)
    {
      if (ks_wifi_dhcp(ip, sizeof(ip)) == 0)
        {
          ks_ui_log("已连接 %s  IP %s", ssid, ip);
          return 0;
        }

      usleep(2 * 1000 * 1000);
    }

  ks_ui_log("已关联但未取到 IP（路由器 DHCP？）");
  return -1;
#else
  ks_ui_log("wifi: WAPI disabled");
  return -ENOSYS;
#endif
}

int ks_wifi_info(FAR char *buf, int buflen)
{
  buf[0] = '\0';
#ifdef CONFIG_WIRELESS_WAPI
  int sock = wapi_make_socket();
  struct in_addr addr;
  if (sock < 0)
    {
      return sock;
    }

  memset(&addr, 0, sizeof(addr));
  if (wapi_get_ip(sock, KS_IF, &addr) == 0)
    {
      snprintf(buf, buflen, "%s", inet_ntoa(addr));
    }

  close(sock);
#endif
  return buf[0] ? 0 : -1;
}
