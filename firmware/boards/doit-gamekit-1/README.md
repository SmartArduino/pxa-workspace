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
| MENU / RETURN | 19 / 18 |
| FT6336U 触摸 SDA / SCL / INT / RESET | 13 / 14 / 15 / 16 |
| 系统 I2C SDA / SCL，预留 | 34 / 33 |
| 测试 LED，低有效，预留 | 47 |
| 电池 ADC，R37/R52 默认 100k/100k 二分压 | 49 |

按键均为按下接地、内部上拉，独立消抖 12 ms，支持同时按键。
GPIO0 是 R 键，下载 BOOT 为 GPIO61；GPIO47 在新板上是 LED，不能继续用作 L。
CH343P 使用 UART0，PXADB 默认 2000000 波特率。

原理图第 1 页的 LED2 经 R47 接 3.3 V，由 GPIO47 低电平点亮。系统将其保留
为 `org.doit.peripherals` 扩展中的 `indicator.status` 逻辑端口（GPIO 种类、
仅输出），不会向应用开放物理引脚重配置。应用完成 UUID/版本协商并取得同名
scope 的权限后，以 OUTPUT 模式打开、初值 0/1；写入恰好一个字节 0 或 1。
后台、退出及撤权会排队关灯，清理完成前不能由其他应用接管。

## 显示、声音和存储

用户提供的参考工程 `game_-console/components/lcd/st7789.c` 明确使用
**ST7789V TN** 初始化；本板逐项采用相同的电源、VCOM、帧率、Gamma 和延时。
`CL32CK305-18A_CTP.rar` 的[规格书](docs/CL32CK305-18A_CTP_SPEC_V0.pdf)
列出 ILI9341V，和参考工程实际使用的屏幕不同，不能用其 IPS 初始化表驱动本板。

显示使用 320×240 RGB565 逻辑画面及双输出缓冲。LVGL 直接更新完整的 PSRAM
逻辑帧（DIRECT 模式），不再使用 8 行分块绘制及保留帧中转拷贝。默认 SPI2 80 MHz、
`COLMOD=0x05`、`MADCTL=0x00`、不反色、RGB、无偏移；保留 TN 屏的电源与 Gamma
配置，软件按 16×16 分块旋转为 240×320 原生扫描顺序，同时转换为高字节在前。
完整输出帧通过 PSRAM 直接 DMA 传输；旋转下一帧与当前帧送屏重叠，传输排空后
才能回收输出缓冲。`PXA_DOIT1_LCD_PSRAM_DMA_DIRECT` 可回退四行内部中转。
GameRender 和 GuestMapped Surface 均可使用直通路径，缩放与旋转合并一次复制。
背光在首帧输出完成后点亮。原理图未连接 TE，软件完成帧率不等同于面板扫描率，
原生扫描顺序避免横屏地址交换带来的斜向接缝，但无法保证消除所有可见撕裂。
`PXA_DOIT1_LCD_FLIP_180` 可同步翻转显示和触摸方向，默认关闭。
有效显示区域约为 65×50 mm（横屏），320×240 对应约 125 DPI。
固件与模拟器默认统一为 125 DPI，可通过 `PXA_DOIT1_DENSITY_DPI` 修改。
系统安全区上、右、下、左均为 0，模拟器使用相同配置。关闭导航栏后，设置页
滚动视口延伸到系统安全区底边，不再保留未显示的导航栏高度；其他板子使用
各自的系统安全区确定这条底边。
MAX98357A 使用 48 kHz、16 位双声道 I2S，单声道 PCM 复制至左右时隙，软件调音量。

新板 SD 接线符合 S31 的 SDMMC slot 0，使用 **四线 SDMMC、20 MHz**。
CMD/DAT0–3 已有外部 10k 上拉，CD 未连接。启动挂载 FAT16/FAT32 卡到
`/pxa/sdcard`，系统和 PXADB 显示为 `/sdcard`；支持 UTF-8 长文件名。
没有卡或挂载失败时继续启动，不自动格式化，不提供热插拔。
内部 LittleFS `pxa_data` 保留系统、安装包、字体和存档，分区布局与旧板相同。

## 操作

手柄路由依据前台 PXA 根节点的 controller 事件订阅，不以游戏 Surface 是否显示判断。应用的 Canvas 菜单继续接收手柄；原生系统弹窗、输入法和最近任务始终优先导航。未订阅 controller 的界面仍用原生焦点，显式鼠标模式保持可用。

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
| MENU + L / R | 游戏或系统界面音量减 / 加 5%，长按每 250 ms 重复 |
| START 在系统界面单按并松开 | 打开/关闭最近任务 |
| SELECT + START 保持 600 ms，仅系统界面 | 返回桌面 |
| MENU + X | 开关兜底鼠标，游戏中也可使用 |
| SELECT + L / R，仅系统界面 | 音量减 / 加 5%，长按每 250 ms 重复 |
| SELECT + Y 保持 1.2 s，仅系统界面 | 启动 `DOIT-GameKit-1` Wi-Fi 配网热点 |

最近任务中，方向键选卡片、A 切换、X 关闭选中的任务、B 或 RETURN 返回。
游戏内普通 SELECT 组合键完整传给模拟器；独立 MENU 音量组合不会触发回桌面
或最近任务，松开组合前也不会向游戏补发肩键。

MENU/RETURN 独立消抖，只用于系统操作，不占用或扩展游戏 controller 协议。
锁屏不显示焦点框；熄屏时第一组按键只亮屏，全部松开后再按一次才解锁。
亮屏锁屏时任意手柄键（含 MENU/RETURN）均可解锁；本次按键和后续加入的键
吞到全部松开，避免误操作桌面或游戏。锁屏前已按住的键必须先松开。
实体按键和 PXADB 手柄注入共用此规则；手柄空闲时保留触摸上滑解锁。

Wi-Fi 开关立即保存到 NVS 的 `pxa_user/wifi`。启动时先读取开关，关闭时不启动
无线扫描或连接；未保存设置时默认开启。SELECT + Y 长按配网会开启 Wi-Fi 并保存
开启状态，不会清除已经保存的网络。

## 电源和硬件验证

电池电压经 R37/R52 接入 GPIO49（ADC1 channel 7）。独立低优先级任务每 2 秒
采样，使用 S31 的 0 dB、line fitting 和带通道的电压校准接口；丢弃首样本，
平均 16 次采样并平滑电压，按普通 4.2 V 单节锂电池曲线估算百分比，发布到系统
状态栏。电量会受负载、温度和电芯影响，不能代替电量计。连续采样失败 10 秒后
标记电量不可用；欠量程、异常电压或接近 ADC 上限时立即标记不可用。

**V0.8 分压的限制**：S31 ADC 只支持 0 dB，单端输入上限约 2 V，而原板
100k/100k 在电池 4.2 V 时输入 2.1 V。因此默认硬件在接近满电时不能可靠测量，
固件在任一样本达到 1.98 V 时停止发布百分比，避免把饱和值当成真实电量。
如需覆盖完整电池电压范围，可将 **R37 改为 120k，R52 保持 100k**，同时将
`PXA_DOIT1_BATTERY_R37_KOHM` 设为 `120`；4.2 V 此时分压为约 1.91 V。
`PXA_DOIT1_BATTERY_R52_KOHM` 默认 `100`；配置必须与实装阻值一致。
相关范围及校准要求见 [S31 ADC 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/peripherals/adc/index.html)。
原理图没有 ADC 输入滤波电容，实测噪声较大时可在 GPIO49 对地加 100 nF。

IP5306 及滑动电源开关由硬件控制，没有连接到 MCU 的充电状态、电池在位或
断电控制线，不发布充电或软件断电能力。仅靠电压不能可靠判断插入 USB 或电池在位。
Type-C 数据线接到 CH343P，`USB+5V` 未接 MCU 检测脚；充满、正在充电和
USB 供电状态均需额外硬件信号，不能用电量百分比或串口是否通信代替。
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
