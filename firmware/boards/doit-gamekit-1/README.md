# DOIT GameKit 1 (V0.8)

Board ID: `doit-gamekit-1`，ESP32-S31，ESP-IDF 6.2，16 MiB Flash。
依据 [V0.8 原理图](docs/SCH_V0.8_2026-10-20.pdf)，从 `doit-gamekit` 派生独立
component、配置、分区表和依赖锁。默认 CPU 320 MHz、PSRAM 250 MHz。

## 接线

| 功能 | GPIO |
| --- | --- |
| LCD SCLK / MOSI / CS / DC / RESET | 12 / 11 / 10 / 9 / 8 |
| 背光，PWM 高有效 | 7 |
| MAX98357A BCLK / LRCLK / DIN | 4 / 5 / 6 |
| SDMMC CLK / CMD / DAT0 / DAT1 / DAT2 / DAT3 | 24 / 25 / 20 / 21 / 22 / 23 |
| 上 / 下 / 左 / 右 | 45 / 42 / 44 / 43 |
| A / B / X / Y | 2 / 17 / 1 / 3 |
| START / SELECT / L / R | 39 / 40 / 46 / 0 |
| MENU / RETURN | 18 / 19 |
| FT6336U 触摸 SDA / SCL / INT / RESET | 13 / 14 / 15 / 16 |
| 系统 I2C SDA / SCL，预留 | 34 / 33 |
| 测试 LED，低有效，预留 | 47 |
| 电池 ADC，100k/100k 二分压，预留 | 49 |

按键均为按下接地、内部上拉，独立消抖 12 ms，支持同时按键。
GPIO0 是 R 键，下载 BOOT 为 GPIO61；GPIO47 在新板上是 LED，不能继续用作 L。
CH343P 使用 UART0，PXADB 默认 2000000 波特率。

## 显示、声音和存储

用户提供的参考工程 `game_-console/components/lcd/st7789.c` 明确使用
**ST7789V TN** 初始化；本板逐项采用相同的电源、VCOM、帧率、Gamma 和延时。
`CL32CK305-18A_CTP.rar` 的[规格书](docs/CL32CK305-18A_CTP_SPEC_V0.pdf)
列出 ILI9341V，和参考工程实际使用的屏幕不同，不能用其 IPS 初始化表驱动本板。

显示使用 320×240 RGB565 双缓冲及 GameRender 直通路径。默认 SPI2 60 MHz、
`COLMOD=0x05`、`MADCTL=0x60`、不反色、RGB、无偏移，以高字节在前的顺序
直接输出 320×240 横屏帧，不做软件旋转。背光在首帧输出后点亮。
`PXA_DOIT1_LCD_FLIP_180` 可同步翻转显示和触摸方向，默认关闭。
规格书有效区域为 48.60×64.80 mm，物理密度约 125 DPI；为保持既有界面布局，
逻辑密度仍用 160 DPI，可通过 `PXA_DOIT1_DENSITY_DPI` 修改。
MAX98357A 使用 48 kHz、16 位双声道 I2S，单声道 PCM 复制至左右时隙，软件调音量。

新板 SD 接线符合 S31 的 SDMMC slot 0，使用 **四线 SDMMC、20 MHz**。
CMD/DAT0–3 已有外部 10k 上拉，CD 未连接。启动挂载 FAT16/FAT32 卡到
`/pxa/sdcard`，系统和 PXADB 显示为 `/sdcard`；支持 UTF-8 长文件名。
没有卡或挂载失败时继续启动，不自动格式化，不提供热插拔。
内部 LittleFS `pxa_data` 保留系统、安装包、字体和存档，分区布局与旧板相同。

## 操作

FT6336U 使用 7 位 I2C 地址 `0x38`、100 kHz，复位拉低 20 ms、释放后等待
300 ms。每 10 ms 读取状态及两个触点记录，接受 DOWN/CONTACT、正确处理 UP
和零触点报告，按跟踪 ID 保持 LVGL 触点编号。模组规格书标注「单点加手势」，
驱动兼容芯片的两个触点记录；实际可同时报告的触点数由模组固件决定。

触摸原生范围为 240×320，按参考工程交换 X/Y 并反转 Y，映射为 `(y, 239-x)`；
开启 `PXA_DOIT1_LCD_FLIP_180` 时映射为 `(319-y, x)`。
每个接触点使用独立 LVGL 输入设备，保留按下/抬起边沿；报文连续失败 100 ms
取消旧手势，防止卡住。触摸时退出按键焦点，按实体键恢复焦点导航；系统切页
取消旧手势，熄屏后的第一组接触只唤醒屏幕。实体触摸和按键优先于 PXADB 注入。
触摸缺失或初始化失败仍可使用按键，`PXA_DOIT1_TOUCH` 可关闭实体触摸。

默认纯按键焦点导航，无底部导航栏；方向键移动，A 确认，B 在系统界面返回，
L/R 顺序切换焦点。显示游戏 Surface 时自动报告 12 个手柄键。

| 按键 | 操作 |
| --- | --- |
| MENU 短按并松开 | 打开/关闭最近任务，游戏中也可使用 |
| MENU 保持 600 ms | 返回桌面，松开不再打开最近任务 |
| RETURN | 返回，游戏中也可使用 |
| START 在系统界面单按并松开 | 打开/关闭最近任务 |
| SELECT + START 保持 600 ms | 返回桌面 |
| SELECT + X | 开关兜底鼠标 |
| SELECT + L / R | 音量减 / 加 5%，长按每 250 ms 重复 |
| SELECT + Y 保持 1.2 s | 启动 `DOIT-GameKit-1` Wi-Fi 配网热点 |

MENU/RETURN 独立消抖，只用于系统操作，不占用或扩展游戏 controller 协议。
熄屏后的第一组按键只唤醒屏幕，全部松开后恢复操作。

## 电源和硬件验证

电池电压经 R37/R52 接入 GPIO49；尚未启用电量估算。IP5306 及滑动电源开关
由硬件控制，没有连接到 MCU 的充电状态或断电控制线，不发布充电或软件断电能力。
屏幕实际显示方向、颜色、触摸四角对应及扬声器声音需结合实体屏幕验证。

## 编译和烧录

从项目根目录执行：

```sh
source "$HOME/esp/esp-idf/esp-idf-main/export.sh"
tools/firmware.sh doit-gamekit-1 build
tools/firmware.sh doit-gamekit-1 flash --port /dev/ttyACM1 --baud 2000000
tools/firmware.sh doit-gamekit-1 monitor --port /dev/ttyACM1 --baud 2000000
```

串口替换为本板 CH343P 的实际设备。构建目录为 `build/firmware/doit-gamekit-1/`，
依赖锁由 Component Manager 生成到 `firmware/dependencies.lock.doit-gamekit-1`。
启动早期 ROM 控制台为 115200，运行期 PXADB 为 2000000。

首次初始化中文字体：

```sh
tools/factory.sh image doit-gamekit-1
python -m esptool --chip esp32s31 --port /dev/ttyACM1 --baud 2000000 \
  write-flash 0x510000 out/pxa-partitions/doit-gamekit-1/pxa_data-empty.bin
```

该命令替换内部数据分区，仅用于首次初始化。日常更新使用
`tools/firmware.sh doit-gamekit-1 app-flash --port ... --baud 2000000`。
应用开发入口为 `tools/dev.sh device --board doit-gamekit-1 store --port ...`，
自动从 `board.cmake` 读取 ESP32-S31 目标。对应的出厂与模拟器 profile 位于
`factory/profiles/doit-gamekit-1.toml` 和 `simulator/profiles/doit-gamekit-1.toml`。

## 验证

2026-10-20：ESP-IDF main 完整构建及 codec memory audit 通过，5 MiB 应用分区
剩余约 24%。板级输入、触摸坐标和工具测试通过，字体出厂镜像生成并写入成功。
在 `/dev/ttyACM1` 验证 ESP32-S31 v0.0、16 MiB Flash/PSRAM、250 MHz PSRAM
内存测试、FT6336U（芯片 ID `0x64`、固件 `0x13`）、1876 MiB 四线 SDMMC 卡
及 320×240 中文桌面截图。屏幕与触摸参数已按用户提供的参考工程统一修正，
最终画面颜色、方向与触摸四角对齐情况需人工核对。

```sh
bash firmware/boards/doit-gamekit-1/tests/test_host.sh
python3 -m unittest discover -s tools -p test_dev.py
python3 -m unittest discover -s tools -p test_build_firmware_release.py
tools/firmware.sh doit-gamekit-1 build
```

板级输入测试覆盖 14 键独立消抖、同时按键、计时回绕、MENU 短按/长按/松开、
RETURN 单次返回、系统键与手柄键并用，以及原有组合键的触发和松开吞键。
触摸测试覆盖两个方向的全部 76,800 个像素、无效报文、重复 ID、DOWN/CONTACT/UP、
零触点、双触点换序、单指抬起及新增触点的槽位保持。输出测试检查 RGB565
高低字节、行顺序、1/2/4 倍缩放、带跨度源缓冲、截屏及裁剪叠加层，均在
ASan/UBSan 下运行。引脚唯一性在编译时检查，含触摸、系统 I2C、LED 和 ADC 引脚。
