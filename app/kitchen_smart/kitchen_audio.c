/****************************************************************************
 * kitchen_audio.c - 录音/播放（v2：线程安全、写文件、不阻塞 UI）
 *
 * 录音：nxrecorder -> 16kHz 单声道 16bit PCM 文件
 * 播放：nxplayer -> wav/pcm 文件
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>

#include <nuttx/audio/audio.h>
#include "system/nxrecorder.h"
#include "system/nxplayer.h"

#include "kitchen_smart.h"

#define KS_REC_DEV  "/dev/audio/pcm0c"
#define KS_PLAY_DEV "/dev/audio/pcm0p"

int ks_audio_record(FAR const char *path, int seconds)
{
  FAR struct nxrecorder_s *rec;
  int ret;

  if (seconds <= 0)
    {
      seconds = 3;
    }

  rec = nxrecorder_create();
  if (rec == NULL)
    {
      ks_ui_log("audio: recorder create failed");
      return -ENOMEM;
    }

  nxrecorder_setdevice(rec, KS_REC_DEV);

  ret = nxrecorder_recordinternal(rec, path, AUDIO_FMT_PCM,
                                  1, 16, 16000, 1);
  if (ret < 0)
    {
      ks_ui_log("audio: record start failed %d", ret);
      nxrecorder_release(rec);
      return ret;
    }

  ks_ui_log("recording %d s ...", seconds);
  sleep(seconds);

  nxrecorder_stop(rec);
  nxrecorder_release(rec);

  ks_ui_log("record done: %s", path);
  return 0;
}

/* 单声道 -> 立体声（每个样点复制一份）。
 * 有些 codec 只接受立体声，裸 PCM 单声道播不出来时用它兜底。 */
static int make_stereo_copy(FAR const char *src, FAR const char *dst)
{
  FILE *fi = fopen(src, "rb");
  FILE *fo;
  short in[512];
  short out[1024];
  size_t n;
  int i;

  if (fi == NULL)
    {
      return -1;
    }

  fo = fopen(dst, "wb");
  if (fo == NULL)
    {
      fclose(fi);
      return -1;
    }

  while ((n = fread(in, sizeof(short), 512, fi)) > 0)
    {
      for (i = 0; i < (int)n; i++)
        {
          out[2 * i]     = in[i];
          out[2 * i + 1] = in[i];
        }

      fwrite(out, sizeof(short), n * 2, fo);
    }

  fclose(fi);
  fclose(fo);
  return 0;
}

/* 播放串行化：两路播放同时进行会把 codec 的采样率互相覆盖、文件互相截断 */
static pthread_mutex_t g_play_lock = PTHREAD_MUTEX_INITIALIZER;

/* 从 WAV 里定位 data 块，把裸 PCM 拷到 pcm 路径。
 * 每次都重新提取：这样既不会播到上一条回答的残留文件，
 * 也不依赖 MQTT 线程是否成功提取过（那正是之前"3 倍速兜底"的来源）。 */
static int extract_pcm(FAR const char *wav, FAR const char *pcm)
{
  FILE *fi;
  FILE *fo;
  unsigned char hdr[1024];
  unsigned char buf[2048];
  size_t n;
  size_t r;
  long off = 12;
  long dstart = -1;

  fi = fopen(wav, "rb");
  if (fi == NULL)
    {
      return -1;
    }

  n = fread(hdr, 1, sizeof(hdr), fi);
  if (n < 44 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
      fclose(fi);
      return -1;
    }

  while (off + 8 <= (long)n)
    {
      unsigned int csz = (unsigned int)hdr[off + 4] |
                         ((unsigned int)hdr[off + 5] << 8) |
                         ((unsigned int)hdr[off + 6] << 16) |
                         ((unsigned int)hdr[off + 7] << 24);

      if (hdr[off] == 'd' && hdr[off + 1] == 'a' &&
          hdr[off + 2] == 't' && hdr[off + 3] == 'a')
        {
          dstart = off + 8;
          break;
        }

      /* 异常块大小（云端流式 WAV 的 data size 常是垃圾值）：
       * 头部缓冲区里不可能有这么大的块，直接放弃，避免 off 回绕越界读 */
      if ((long)csz > (long)n)
        {
          break;
        }

      off += 8 + (long)csz + (long)(csz & 1);
    }

  if (dstart < 0 || fseek(fi, dstart, SEEK_SET) != 0)
    {
      fclose(fi);
      return -1;
    }

  fo = fopen(pcm, "wb");
  if (fo == NULL)
    {
      fclose(fi);
      return -1;
    }

  long written = 0;

  /* 不信任头里的 csize（云端流式 WAV 常写成异常值），一直拷到文件尾 */
  while ((r = fread(buf, 1, sizeof(buf), fi)) > 0)
    {
      if (fwrite(buf, 1, r, fo) != r)
        {
          fclose(fi);
          fclose(fo);
          return -1;
        }

      written += (long)r;
    }

  fclose(fi);
  fclose(fo);
  return (written > 1024) ? 0 : -1;
}

/* 16k 单声道 -> 48k 立体声（线性插值 3 倍上采样，再复制到左右声道）。
 * 为什么这么做：实测该 codec 在 48k/立体声下能正常出声，
 * 而直接按 16k 播放没有声音（驱动这一路有问题）。
 * 16k->48k 三倍上采样后按 48k 播，时长/音调仍然正确。 */
static int convert_to_48k_stereo(FAR const char *src, FAR const char *dst)
{
  FILE *fi = fopen(src, "rb");
  FILE *fo;
  short in[512];
  short out[512 * 6];
  size_t n;
  int i;

  if (fi == NULL)
    {
      return -1;
    }

  fo = fopen(dst, "wb");
  if (fo == NULL)
    {
      fclose(fi);
      return -1;
    }

  while ((n = fread(in, sizeof(short), 512, fi)) > 0)
    {
      int o = 0;

      for (i = 0; i < (int)n; i++)
        {
          /* 线性插值 3 倍上采样：比"重复样点"平滑，避免高频滋滋声 */
          int s0 = in[i];
          int s1 = (i + 1 < (int)n) ? in[i + 1] : in[i];
          short v0 = (short)s0;
          short v1 = (short)((2 * s0 + s1) / 3);
          short v2 = (short)((s0 + 2 * s1) / 3);

          out[o++] = v0;      /* L */
          out[o++] = v0;      /* R */
          out[o++] = v1;
          out[o++] = v1;
          out[o++] = v2;
          out[o++] = v2;
        }

      if (fwrite(out, sizeof(short), o, fo) != (size_t)o)
        {
          fclose(fi);
          fclose(fo);
          return -1;
        }
    }

  fclose(fi);
  fclose(fo);
  return 0;
}

int ks_audio_play(FAR const char *path)
{
  FAR struct nxplayer_s *player;
  int ret;
  int dret;

  /* 串行化播放：避免两路播放同时进行把 codec 采样率互相覆盖。
   * 但用 trylock 限时等待——万一某次播放卡住不返回，
   * 也不能把后续所有播放永久堵死（那就是"点了没声音"）。 */
  int locked = 0;
  int wait;

  for (wait = 0; wait < 20; wait++)
    {
      if (pthread_mutex_trylock(&g_play_lock) == 0)
        {
          locked = 1;
          break;
        }

      usleep(500 * 1000);
    }

  if (!locked)
    {
      ks_ui_log("上次播放未结束，仍继续本次播放");
    }

  player = nxplayer_create();
  if (player == NULL)
    {
      ks_ui_log("播放器创建失败");
      if (locked)
        {
          pthread_mutex_unlock(&g_play_lock);
        }
      return -ENOMEM;
    }

  dret = nxplayer_setdevice(player, KS_PLAY_DEV);
  if (dret < 0)
    {
      ks_ui_log("%s 打开失败(%d)", KS_PLAY_DEV, dret);
    }

  /* 音量：驱动只在录音路径设过 ADC 音量，播放 DAC 音量原本是复位默认值 */
  if (nxplayer_setvolume(player, 1000) < 0)
    {
      ks_ui_log("设置音量失败（音量支持可能未开启）");
    }

  /* 路径 1（首选）：提取裸 PCM -> 升采样成 48k 立体声 -> 按 48k/2ch 播。
   * 实测该 codec 在 48k/立体声下能出声；直接 16k 播放无声（驱动那一路有问题）。
   * 16k->48k 是 3 倍上采样，时长与音调保持正确，不会出现"3 倍速"。 */
  int pret = -1;

  if (extract_pcm(path, KS_TTS_PCM) != 0)
    {
      ks_ui_log("本次音频提取失败(%s)", path);
    }
  else if (convert_to_48k_stereo(KS_TTS_PCM, KS_TTS_PCM2) != 0)
    {
      ks_ui_log("音频转换失败");
    }
  else
    {
      pret = nxplayer_playraw(player, KS_TTS_PCM2, AUDIO_FMT_PCM, 0,
                              2, 16, 48000, 1);
      if (pret == 0)
        {
          ks_ui_log("正在播放 48k 立体声");
          nxplayer_release(player);
          if (locked)
            {
              pthread_mutex_unlock(&g_play_lock);
            }

          return 0;
        }

      ks_ui_log("48k 播放失败(%d)，试 16k 单声道", pret);

      /* 路径 2：直接按 16k 单声道播原声 */
      pret = nxplayer_playraw(player, KS_TTS_PCM, AUDIO_FMT_PCM, 0,
                              1, 16, 16000, 1);
      if (pret == 0)
        {
          ks_ui_log("正在播放 16k 单声道");
          nxplayer_release(player);
          if (locked)
            {
              pthread_mutex_unlock(&g_play_lock);
            }

          return 0;
        }
    }

  ret = pret;
  ks_ui_log("播放失败(%d)：音频格式不支持", ret);

  nxplayer_release(player);

  if (locked)
    {
      pthread_mutex_unlock(&g_play_lock);
    }

  return ret;
}

/* 录音并上传云端（AI 语音请求） */
static void *record_upload_thread(FAR void *arg)
{
  int seconds = (int)(intptr_t)arg;
  FILE *f;
  long size;
  char *buf;
  int ret;

  if (ks_audio_record(KS_REC_PCM, seconds) != 0)
    {
      return NULL;
    }

  f = fopen(KS_REC_PCM, "rb");
  if (f == NULL)
    {
      ks_ui_log("open %s failed", KS_REC_PCM);
      return NULL;
    }

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > (512 * 1024))
    {
      ks_ui_log("录音大小异常(%ld)，已丢弃", size);
      fclose(f);
      return NULL;
    }

  buf = malloc(size);
  if (buf == NULL)
    {
      fclose(f);
      return NULL;
    }

  if (fread(buf, 1, size, f) != (size_t)size)
    {
      fclose(f);
      free(buf);
      return NULL;
    }

  fclose(f);

  ks_ui_log("上传中 %ld 字节 ...", size);
  ret = ks_mqtt_publish_ai_pcm(buf, (int)size);
  free(buf);

  if (ret < 0)
    {
      /* 之前这里无论成败都显示"uploaded"，把失败伪装成成功 */
      ks_ui_log("上传失败 ret=%d（检查 MQTT 是否连接）", ret);
    }
  else
    {
      ks_ui_log("已上传 %ld 字节，等待 AI ...", size);
    }

  return NULL;
}

int ks_audio_record_and_upload(int seconds)
{
  pthread_t tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 12288);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  int ret = pthread_create(&tid, &attr, record_upload_thread,
                           (FAR void *)(intptr_t)seconds);
  pthread_attr_destroy(&attr);
  return ret;
}
