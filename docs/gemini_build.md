# Gemini-S1 工程接入与构建

目标平台为 Allwinner R528 / Gemini-S1、2.8 英寸 ILI9341 屏，使用 openvela 的 `dev-ai-contest-2026` 分支。需在已经配置交叉编译环境的 Linux openvela 工作区执行构建。单独克隆本队仓库不包含完整 BSP、工具链和基础系统。

## 拉取与应用接入

按根目录 README 的 `repo init` 和 `repo sync` 命令拉取完整工程。清单将本队 `app/kitchen_smart/` 软链到 `packages/demos/contest2026_122_kitchen_smart`。

`packages/demos/Make.defs` 会发现其 `Make.defs`，`CONFIG_KITCHEN_SMART` 控制应用是否参与编译。应用 Makefile 引用厂商的 `vendor/allwinnertech/Common.mk` 与 `AW_ADD_HAL_INCLUDES`，依赖 Allwinner BSP，不能作为普通 PC C 程序编译。

旧交付说明使用 `vendor/allwinnertech/apps/kitchen_smart/`；本次入仓已通过清单接入 `packages/demos/`，不要再重复复制到旧目录。

## 板级配置

交付包提供的是配置差异说明。以官方 `nsh_minidisplay` 为基础创建厨房配置，再按照 [板级配置说明](board/板级配置说明.md) 设置功能项。在 openvela 工作区根目录执行：

```bash
mkdir -p vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/kitchen
cp -n vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig \
  vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/kitchen/defconfig
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/kitchen/ menuconfig
```

启用 `CONFIG_KITCHEN_SMART`，关闭 `CONFIG_LUNCHER_MINI_APP`，设置 MQTT 地址、端口、设备 ID 和 `wlan0`。按板级说明核对 LVGL、中文字体、WiFi/DHCP、录音播放及音量控制配置，关闭 `CONFIG_AUDIO_EXCLUDE_VOLUME`，保存生成的配置。

应用需要中文字体资源 `/resource/fonts/MiSans-Normal.ttf`。完整配置项与开机启动片段见上述板级说明。

## 音频与启动

按 [音频驱动修改说明](audio/驱动补丁说明.md) 检查播放路径的 DAC 音量初始化，以及 0～1000 至 0～255 的音量换算。该目录保存修改说明和代码片段，不包含完整 `sunxi_alsa.c`；应根据所用 BSP 的原文件应用修改。公共仓库修改按赛事规则向相应仓库的比赛分支发起 PR。

可先在 NSH 手工执行 `kitchen_smart`；需要开机启动时，按板级说明将条件启动片段加入对应 `rcS`。

## 编译、打包与烧录

在 openvela 工作区根目录执行：

```bash
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/kitchen/ -j8

cd vendor/allwinnertech/lichee
./tools/scripts/pack_img.sh -c sun8iw20p1 -p rtos -b r528s3-gemini-s1 -o nuttx \
  -d uart0 -s none -m normal -w none -v none -i none -t "$PWD" \
  -f r528s3/gemini-s1_nand -g r528s3/gemini-s1_nand
```

交付说明中的输出路径（相对于 `vendor/allwinnertech/lichee/`）：

```text
out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```

打包参数来自交付包，适用于其中的 Gemini-S1 NAND 方案。通过 PhoenixSuit 选择生成镜像，按开发板的 FEL 流程烧录。源码仓库不包含交付包中的旧版 `.img`。

## 设备与网关参数

设备可用 `/data/etc/kitchen.conf` 覆盖编译期默认值：

```ini
host = broker.emqx.io
port = 1883
device_id = device001
wifi_if = wlan0
```

网关 `gateway/config.ini` 使用同一 broker、端口及设备 ID。WiFi 配网由设备界面完成。

本次整理静态核对了清单软链与应用注册路径，保留交付的 C 源码、Kconfig 和 Makefile；未执行完整 openvela 交叉编译或烧录。复现时须结合实际 BSP、驱动版本、字体资源和硬件构建。
