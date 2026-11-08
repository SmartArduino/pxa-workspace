# DOIT ESP32-S31 DevKit

Board ID：`doit-esp32s31-devkit`。使用 ESP-IDF 6.2、ESP32-S31、8 MiB Flash，
CPU 320 MHz、16 MiB Octal PSRAM 250 MHz。按 `docs/ESPS31-32_DevKit_v1.0.pdf`
接线，屏幕为 ST7102、480×854、4.96 英寸，显示密度 198 DPI。

## 接线与共享引脚

| 功能 | GPIO |
| --- | --- |
| 共用 I2C0 SDA / SCL | 0 / 1 |
| LCD SPI SDA / SCL / CS / SDO | 0 / 1 / 61 / 60 |
| LCD 与触摸共用 RESET | 9 |
| LCD RGB565 B3–B7 | 57 / 7 / 5 / 6 / 8 |
| LCD RGB565 G2–G7 | 51 / 52 / 53 / 54 / 55 / 56 |
| LCD RGB565 R3–R7 | 36 / 35 / 34 / 33 / 50 |
| LCD DE / PCLK / HSYNC / VSYNC | 44 / 43 / 42 / 40 |
| 背光 LP3321 EN，高有效 PWM | 37 |
| 触摸 INT，当前轮询 | 38 |
| ES8311 MCLK / BCLK / LRCK | 49 / 48 / 46 |
| ES8311 MCU DOUT / DIN | 47 / 45 |
| NS4150 功放使能，高有效 | 39 |
| SDMMC CLK / CMD / D0 / D1 / D2 / D3 | 24 / 25 / 20 / 21 / 22 / 23 |
| SD 电源使能，低有效 | 14 |
| UART0 TX / RX，CH340X USB-Uart | 58 / 59 |

GPIO0/1 同时连接屏幕控制 SPI、触摸 I2C、音频 I2C 和摄像头 SCCB。
启动时先用开漏 GPIO 发送屏幕的 9 位 SPI 命令，结束后保持 CS 为高、恢复总线，
再创建 I2C0。运行中不再发送屏幕 SPI 命令，触摸初始化不重复复位共用的 GPIO9。
原理图的 `LCD_MOSI` 实际连接模组 SDO，GPIO60 保留为输入，不作为数据输出。

BOOT 与 LCD_CS 共用 GPIO61，固件以开漏方式释放片选，避免按键对推挽输出短路。
BOOT 只用于复位下载，运行中不要按住：它会选中屏幕，使其他 I2C 流量进入屏幕
SPI 接口。系统操作使用触摸导航和 PXADB，不将 BOOT 注册为返回或 Home 键。

## 显示、触摸与音频

屏幕命令表沿用仓库中经过 Apache-2.0 授权的 ST7102 寄存器配置，与所提供的
`r050hx_st7102_480x854.c` 表一致；通过 SPI 控制、RGB 并行送像素。
总线连接各通道的高位，面板使用 24 位输入格式。LCD 外设配置 24 个输出时隙，
8 个未接引脚设为 NC，由硬件将 RGB565 帧缓冲转换为 RGB888 后输出；不能将
24 位面板格式与 16 位外设时隙混用。
时序采用资料中的 PNG：36 MHz，水平同步/后肩/前肩 5/40/40，
垂直同步/后肩/前肩 5/9/193，像素时钟下降沿输出。

LVGL FULL 模式直接绘制两个 PSRAM 帧缓冲，合成 PXA Surface 与系统覆盖层后提交。
RGB 驱动的 buffer complete 事件负责归还缓冲，VSYNC 独立计时；截图在缓冲交接
后的两个 VSYNC 后读取，避免把提交时间当成完整扫描时间。首帧完成前保持背光关闭。

触摸按 `ST7102 TDDI Interface Protocol V01.01` 独立实现，I2C 7 位地址 `0x55`、
100 kHz、16 位寄存器地址。独立的 6 针触摸排线必须接入 JP4；仅接 40 针 LCD
排线无法读取触摸，补接后需重启。读取固件报告的坐标范围和触点数，最多保留 10 个固定
触点槽位；抬起或交叉时不重新编号。连续读取失败 150 ms 后取消手势。
不链接资料 ZIP 内的 Linux 驱动，不新增 copyleft 系统依赖。

ES8311 使用地址 `0x18`、外部 MCLK、16 kHz/16 位双声道 I2S。
单声道播放复制到两个时隙，音量通过 codec 控制；麦克风在应用获得权限并打开资源后
才启用 RX。初始化失败时仍可使用显示和 PXADB。

SD 使用 slot 0、四线、20 MHz，启动时探测一次 FAT16/FAT32 卡，挂载到 `/sdcard`。
卡检测未连接，不支持热插拔；挂载失败不会格式化卡。没有电池检测和充电状态信号，
不发布电池百分比或软件断电能力。


## 面板现场比较

`-DPXA_DOIT_S31_PANEL_DIAGNOSTICS=ON` 仅用于板级排查，默认关闭。
启用后，PXADB 测试键 `volume-up` 切换 PCLK 输出极性，`volume-down`
依次切换 36/30/24/20 MHz，`home` 依次切换系统界面/纯黑/中灰/高对比图案，
`back` 依次切换 75/25/5% 背光；这些诊断键会替代通常的系统测试键行为。
诊断期间保持显示活动，不修改保存的暗屏设置。每次复位恢复基准参数。
电压、VCOM 和 Gamma 表保持原值，比较结果须观察实体屏幕；帧缓冲截图不能
证明实体屏幕的残影或边缘发白已消除。恢复普通构建时显式传入
`-DPXA_DOIT_S31_PANEL_DIAGNOSTICS=OFF`。

## 摄像头接口

JP1 标注为 DVP-OV5640；D0–D7 为 13/11/10/12/15/17/18/4，
VSYNC/HSYNC/MCLK/PCLK 为 2/3/19/16。RESET 经电阻拉高，PWDN 拉低，均未接 MCU。
所给 `ST2510-DJ-05-20A6-A-V2` 资料标注 BF20A6，24 针排布与 JP1 不同，
不能仅因针数相同直接插入。当前不启用摄像头；需先确认实际传感器与转接接线，
并具备允许用于系统的驱动和寄存器资料后再接入。

## 构建与烧录

从仓库根目录执行，串口使用本板的 USB-Uart：

```sh
source "$HOME/esp/esp-idf/esp-idf-main/export.sh"
tools/firmware.sh doit-esp32s31-devkit build
tools/firmware.sh doit-esp32s31-devkit flash --port /dev/ttyUSB0 --baud 921600
tools/firmware.sh doit-esp32s31-devkit monitor --port /dev/ttyUSB0 --baud 921600
```

ROM 控制台为 115200；应用控制台和 PXADB 为 921600。
构建使用 `build/firmware/doit-esp32s31-devkit/`，依赖锁由 Component Manager
生成到 `firmware/dependencies.lock.doit-esp32s31-devkit`。
开发入口 `tools/dev.sh device --board doit-esp32s31-devkit <app> --port /dev/ttyUSB0`
自动选择 S31 AOT 目标和串口波特率。

Flash 分区：应用位于 `0x10000`、大小 5 MiB；LittleFS `pxa_data`
位于 `0x510000`、大小 `0x2f0000`，挂载到 `/pxa`。中文字体约占 2.7 MiB，
内部余量有限，应用安装和较大文件应使用 SD 卡卷。
首次初始化中文字体可生成镜像并写入该分区；这会替换其内容，仅用于首次部署：

```sh
artifact_dir="local/artifacts/doit-esp32s31-devkit/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$artifact_dir"
tools/factory.sh image doit-esp32s31-devkit --output "$artifact_dir/pxa-data-font.bin"
python -m esptool --chip esp32s31 --port /dev/ttyUSB0 --baud 921600 \
  write-flash 0x510000 "$artifact_dir/pxa-data-font.bin"
```

日常更新使用 `app-flash` 保留数据。模拟器 profile 为
`simulator/profiles/doit-esp32s31-devkit.toml`，与实机共用分辨率、密度及导航布局。

板级测试运行 `bash firmware/boards/doit-esp32s31-devkit/tests/test_host.sh`，
覆盖合成、透明覆盖层、触摸槽位、报文错误和扫描边界；运行产物写入 `local/artifacts/`。
