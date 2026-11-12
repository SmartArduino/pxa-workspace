# DOIT GameKit 2 (V2.0)

维护中的 GameKit 板型，Board ID 为 `doit-gamekit-2`。ESP32-S31、ESP-IDF 6.2、
16 MiB Flash/PSRAM，默认 CPU 320 MHz、PSRAM 250 MHz。
硬件依据 [V2.0 原理图](docs/SCH_V2.0_2026-11-10.pdf) 和
[电池采样参考参数](docs/lQLPJyFyvbjorynNAXzNAXqwZ-qFYo92tusKxAFeuE7wAA_378_380.png_720x720.jpg)。

`doit-gamekit`、`doit-gamekit-1` 已停止维护和后续固件发布，源码与历史配置保留。
各版引脚和电源电路不同，固件不能跨硬件版本混刷。

## 接线与外设

| 功能 | GPIO / 配置 |
| --- | --- |
| ST7789V LCD SCLK / MOSI / CS / DC / RESET | 12 / 11 / 10 / 9 / 8 |
| 背光 PWM，高有效 | 7 |
| MAX98357A BCLK / LRCLK / DIN | 4 / 5 / 6 |
| 四线 SDMMC CLK / CMD / DAT0 / DAT1 / DAT2 / DAT3 | 24 / 25 / 20 / 21 / 22 / 23 |
| 上 / 下 / 左 / 右 | 45 / 42 / 44 / 43 |
| A / B / X / Y | 2 / 17 / 1 / 3 |
| START / SELECT / L / R | 39 / 40 / 46 / 0 |
| MENU / RETURN | 19 / 18 |
| FT6336U SDA / SCL / INT / RESET | 13 / 14 / 15 / 16 |
| 系统 I2C SDA / SCL，预留 | 34 / 33 |
| LED2，低有效 | 47 |
| VBAT_ADC，ADC1 channel 7 | 49 |
| USB_POWER，USB 接入为低，拔出为高 | 51 |

显示为 320×240 RGB565、125 DPI、无安全边距、默认无底部导航栏。LCD 使用
ST7789V TN 初始化和 80 MHz SPI，以 240×320 原生扫描顺序送屏，软件旋转与
字节转换合并；双输出缓冲直接从 PSRAM DMA 送屏。触摸为 FT6336U，地址
`0x38`，原始 240×320 坐标映射为 `(y, 239-x)`。配置
`CONFIG_PXA_DOIT2_LCD_FLIP_180` 可同步翻转显示与触摸。

MAX98357A 为 48 kHz、16 位双声道 I2S。SD 卡为四线 SDMMC、20 MHz，
FAT16/FAT32，显示路径 `/sdcard`，实际挂载 `/pxa/sdcard`；不自动格式化，
不提供热插拔。内部 LittleFS `pxa_data` 保存系统资源、应用和存档。有 SD 卡时，
系统可校验并迁移字体及许可证到 `/sdcard/PXA/system/fonts`，再回收内部字体副本。
LED2 通过 `org.doit.peripherals` 的 `indicator.status` 逻辑端口使用。

默认接入 `org.doit.wifi-location` 的 `wifi.scan` 权限接口，复用现有 Wi-Fi
管理器，在独立任务中按需读取附近热点快照；没有应用查询时不启动扫描。
可用 `-DPXA_ENABLE_WIFI_LOCATION=OFF` 关闭，应用自行选择定位服务。
显示方向接口声明为原生横屏，不接受运行期竖屏或旋转请求。

## 电池与 USB 外部供电

R37 为 **120kΩ**、R52 为 **100kΩ**，C50 为 100 nF。Q5/Q6 在系统 3.3 V
建立后自动接通采样分压，关机后断开，无需 GPIO 控制。
GPIO49 使用 S31 的 0 dB ADC 和带通道的 line-fitting 校准，丢弃首样本后
平均 16 次采样，再平滑电压。ADC 与 Wi-Fi PHY 位于同一核心，避免模拟
寄存器锁的跨核冲突。按参考表校正，最后一步四舍五入到 mV：

```text
电池电压(mV) = round(ADC校准电压(mV) × (120+100)/100 × 1089/1000) + 127
```

例如 ADC 校准结果 1700 mV 对应约 4200 mV。Kconfig 参数以
`CONFIG_PXA_DOIT2_BATTERY_` 为前缀，分别为 `R37_KOHM`、`R52_KOHM`、
`GAIN_PERMILLE` 和 `OFFSET_MV`。增益/偏移是参考板的测量补偿，应结合实板
万用表核对；设为 `1000` / `0` 可关闭额外补偿。电量按 4.2 V 单节锂电池
曲线估算，受负载、温度和电芯影响，不能代替电量计。ADC 饱和或校正后超出
2500–4350 mV 时立即停止报告电量；连续采样失败 10 秒后清除旧电量。
ADC 每 2 秒采样一次。

GPIO51 读取 Q4 集电极，R51 已外接 10k 上拉。固件仅设为输入，不开启内部
上下拉，不驱动引脚。每 50 ms 检查一次，连续稳定至少 50 ms 后更新状态；
检测独立于 ADC，电池采样失败仍可报告外部供电。按本产品约定，USB 接入
代表充电：状态栏只显示一个 **充电图标**，电量不可用时也显示；拔出后恢复
电池图标。默认不显示电量百分比。

GPIO51 **只检测 USB 外部电源存在**，无法区分实际充电、充满、电池缺失或
电源线是否有数据连接。IP5306 没有向 MCU 接出充电完成或电池在位信号，
`charging` 按产品约定跟随 USB 接入状态，不从百分比推断充满。系统同时发布
`external_power_supported` / `external_power_connected`。SW5 将 IP5306 KEY
拉低，仅用于硬件唤醒升压；SW2 为硬件电源开关，不提供软件断电能力。

## 操作

方向键移动焦点，A 确认，B / RETURN 返回，L/R 顺序切换焦点。
MENU 短按打开最近任务，长按 600 ms 返回桌面；游戏中同样可用。
MENU + L/R 调音量，MENU + X 开关兜底鼠标。系统界面可用 START 打开最近
任务、SELECT + START 长按回桌面、SELECT + L/R 调音量。
SELECT + Y 长按 1.2 秒开启 `DOIT-GameKit-2` Wi-Fi 配网热点。
最近任务中方向键选卡片、A 切换、X 关闭、B / RETURN 返回。
熄屏后的第一组触摸/按键只亮屏；亮屏锁屏时任意手柄键解锁，吞掉本次按键
直至全部松开。游戏手柄保持 12 键，MENU/RETURN 仅用于系统操作。

## 构建与烧录

从仓库根目录运行：

```sh
source "$HOME/esp/esp-idf/esp-idf-main/export.sh"
tools/firmware.sh doit-gamekit-2 build
tools/firmware.sh doit-gamekit-2 flash --port /dev/ttyACM0 --baud 2000000
```

首次安装还需初始化系统字体，会覆盖内部 `pxa_data`，仅在首次安装时运行：

```sh
tools/factory.sh image doit-gamekit-2
python -m esptool --chip esp32s31 --port /dev/ttyACM0 --baud 2000000 --no-stub \
  write-flash 0x510000 out/pxa-partitions/doit-gamekit-2/pxa_data-empty.bin
```

日常更新用 `tools/firmware.sh doit-gamekit-2 app-flash --port ... --baud 2000000`，
保留应用、字体与数据。构建目录为 `build/firmware/doit-gamekit-2/`，依赖锁为
`firmware/dependencies.lock.doit-gamekit-2`。CH343P 使用 UART0，PXADB 和运行期
控制台为 2000000 波特率，ROM 早期控制台为 115200。应用开发可用
`tools/dev.sh device --board doit-gamekit-2 store --port /dev/ttyACM0`。

板级回归测试：`bash firmware/boards/doit-gamekit-2/tests/test_host.sh`，包括
输入、触摸、送屏布局、电压校正、USB 有效电平、去抖及 ADC 故障隔离。
