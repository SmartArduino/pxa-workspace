# doit游戏机 / DOIT GameKit

Board ID: `doit-gamekit`. Target: **ESP32-S31** (`esp32s31`, RISC-V),
ESP-IDF 6.2, 16 MiB W25Q128 flash and in-package PSRAM. This profile follows
the supplied V0.6 schematic. There is no touch controller.

## 接线

| 功能 | GPIO |
| --- | --- |
| ST7789 SCLK / MOSI / CS / DC / RESET | 12 / 11 / 10 / 9 / 8 |
| 屏幕背光，PWM 高有效 | 7 |
| MAX98357A BCLK / LRCLK / DIN | 4 / 5 / 6 |
| SD CLK / CMD / DAT0 / DAT1 / DAT2 / DAT3 | 20 / 21 / 22 / 23 / 24 / 25 |
| 上 / 下 / 左 / 右 | 46 / 38 / 40 / 39 |
| A / B / X / Y | 16 / 15 / **1** / **13** |
| START / SELECT / L / R | 18 / 17 / 47 / 0 |

所有手柄键按下接地，启用内部上拉，独立消抖 12 ms，支持同时按键。
GPIO0 是 R 键；ESP32-S31 的下载 BOOT 是 GPIO61，二者不同。
CH343P 连接 UART0，使用 USB 转串口烧录和 PXADB。

## 性能浮层

在「设置 → 关于设备」中连续确认「系统版本」7 次，展开开发者选项，
再开启「性能浮层」。支持方向键移动焦点、A 切换开关，重启后保留设置。
浮层显示在屏幕顶部中央，最后合成到软件旋转后的输出帧中，游戏 Surface
也不会遮住它；关闭时恢复原有画面。

FPS 统计真正完成 LCD DMA 的内容刷新，每秒更新；没有内容更新时可以降到
`FPS00.0`，系统状态等实际重绘也会计入。浮层自身刷新和 PXADB 为截图
强制触发的重绘不计入 FPS，
以免诊断功能反过来抬高读数。性能日志可以独立开关；采样、NVS 和日志
都在 UI 任务处理，不在 DMA 中断里执行。

## 显示、声音和 SD 卡

ST7789 使用 SPI2、80 MHz、320×240 横屏、RGB565。LVGL 使用两块按
`LV_DRAW_BUF_ALIGN=64` 对齐的 8 行内部绘制缓冲，UI 脏区域保留到一块 PSRAM
逻辑帧；软件旋转输出使用两块 64 字节对齐的 240×320 PSRAM 帧。
独立送屏任务使用 4 行内部 DMA 中转缓冲，完整排空 SPI 队列后才回收输出帧。
旋转复制完成即可释放 LVGL 绘制缓冲，下一帧处理与上一帧送屏重叠。

全屏 RGB565 GameRender 在没有系统弹窗、透明图层或鼠标时直接旋转 Surface，
合并 1×/2×/4× 缩放与字节序转换，跳过 LVGL 全屏合成和逻辑帧中转。新帧通过
任务通知唤醒显示线程；弹窗、最近任务、鼠标和非直通 Surface 使用 LVGL 合成，
关闭弹窗后遵守 Surface 的新完整帧屏障。系统界面仍支持预乘 ARGB 和透明图层。
送屏统计与截图取自实际完成的原生输出帧。背光在首帧写完后点亮，支持亮度、
空闲调暗和熄屏。比较和测量见 [显示性能记录](docs/display-performance.zh-CN.md)。

显示初始化后立即绘制深色 `PXA / Starting...` 启动页，使用内置字体，
不等待 SD、音频、Wi-Fi 或系统字体。整帧送入 LCD 后才打开背光；
系统界面创建完成后移除启动页并主动刷新。早期 115200 控制台中的
`Boot frame visible` 与 `Startup ... ready` 分别记录首屏和各初始化阶段耗时，
单位为启动后的毫秒。

**横屏使用软件旋转，屏幕保持原生 240×320 扫描方向。** 参考 pai-touch 的
整帧输出方式，先收集所有逻辑脏区域并合成 Surface/鼠标，再将完整 320×240
画面按 16×16 分块旋转，同时转换 RGB565 字节序，最后用一次 RAMWR 连续发送
240×320 原生帧。SPI 可分包；送屏任务通过无命令的队列排空操作确认整帧完成，
之后才复用输出帧。没有 XY 交换或镜像命令，MADCTL 的 MV/MX/MY 保持为 0。
本板未接 TE，不进行 TE 等待；原生扫描顺序用于改善命令横屏造成的斜向撕裂，
但仍可能出现横屏上的竖向撕裂，不能保证无撕裂。小区域更新也会发送完整帧。
截图从已传输的原生输出帧反向旋转，向 PXADB 保持提供 320×240 横屏图像。
默认软件旋转 270°、反色、偏移为 0。实际模组若需要将画面转 180°，可选择
软件 90°；方向、RGB/BGR、反色或原生坐标偏移在
`menuconfig → DOIT GameKit` 配置，不需要改引脚或逻辑分辨率。
原理图没有标明有效显示区尺寸，固件与模拟器暂用逻辑 160 DPI。

MAX98357A 使用标准 Philips I2S，16 kHz、16 位双声道时隙；PXA 的单声道
PCM 复制到左右声道。软件缩放音量，0% 输出静音。功放无需 I2C、MCLK 或
独立使能 GPIO。三个 320 帧 DMA 块，每块 20 ms，由阻塞 I2S 写入驱动播放节奏。
参见 [MAX98357A 官方资料](https://www.analog.com/en/products/max98357a.html)。

**SD 卡使用 SPI3 的 SPI 模式，20 MHz。** 原理图的 GPIO20–25 信号顺序与
ESP32-S31 固定 SDMMC IO MUX 不同，不能直接套用四线 SDMMC：S31 slot 0
固定 CLK=24、CMD=25、D0=20、D1=21、D2=22、D3=23。本板按现有接线使用
CLK=20、MOSI/CMD=21、MISO/D0=22、CS/D3=25；D1=23、D2=24 保持输入上拉。
屏幕和卡使用独立 SPI 总线。参见
[ESP32-S31 SDMMC 说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s31/api-reference/peripherals/sdmmc_host.html)。

FAT 卡在启动时挂载到 `/pxa/sdcard`，系统文件管理器和 PXADB 中显示为
`/sdcard`。启用 UTF-8 长文件名。使用 FAT16/FAT32；插卡后重启挂载，不承诺热插拔。没有卡或文件系统
损坏不影响启动，**不会自动格式化卡**。系统、应用安装、字体和私有存档继续放在
内部 LittleFS `pxa_data`，避免拔卡导致系统无法运行。
支持 FS 0.3 的应用可经系统选择器取得 SD 只读目录授权；这不改变安装位置。
统一 `/PXA` 目录及无 Flash 文件分区的 SD 系统盘是下一步后端设计，见
[固定目录与 SD 系统盘](../../../docs/pxa-storage-layout.zh-CN.md)。
本板默认关闭内部 LittleFS 挂载失败自动格式化；首次初始化仍按下文显式写入字体镜像。
IP5306 电源开关由硬件控制，当前不发布电池百分比或软件断电能力。

## 无触摸操作

默认使用 **纯按键焦点导航**，显示琥珀色焦点框，**不显示底部导航栏或三按键**。
焦点框画在控件内侧，并覆盖主题的键盘焦点样式，避免在父容器边缘被裁掉。
方向键按控件位置移动焦点，长按重复；超出列表可视区域时自动滚动。A 确认，
B 逐级返回（先关闭键盘、弹窗或下拉列表，再返回页面/上级目录）。系统界面
单按并松开 START 打开/关闭最近任务，SELECT+START 返回桌面。

L/R 在控件间顺序切换，可以离开滑块或键盘。滑块用左右键调整，松开应用数值；
下拉框用 A 打开、方向键选择、A 确认、B 取消。文本框先用 A 打开屏幕键盘：
方向键按实际字键位置移动；上下可跨行、进入候选栏或底部工具栏，L/R 切换
键盘区域，A 输入/选择候选词，B 收起。焦点框高亮具体字键，不框住整个键盘面板。
系统内置键盘与 PXA Provider 使用的共享键盘均支持这些操作；PXA 键盘打开时
焦点限制在其内部，B 先收起键盘，再逐级返回页面。
仅移动到文本框不会自动弹出键盘。焦点被限制在当前弹窗或锁屏内。
熄屏后的第一组按键只负责唤醒；松开后，再用 A 确认标准锁屏。

**兜底鼠标：SELECT + X 随时切换。** 显示小圆点后，方向键移动指针，A 按下/
松开对应触摸或鼠标的按下/抬起；按住 A 配合方向键可拖动、滚动和滑动。
再次 SELECT+X 返回自动模式。它适用于自绘 Canvas、仅支持触摸的游戏或缺少
可聚焦控件的界面。两种模式切换、页面删除或弹窗打开时会取消旧按下状态，
避免误点新的控件。

显示 PXA 游戏 Surface 时自动进入手柄模式，向 controller 0 发送完整按键状态，
同时支持方向、A/B/X/Y、L/R、START/SELECT。触摸操作的游戏可以切到光标模式。

| 组合键 | 操作 |
| --- | --- |
| SELECT + START 保持 600 ms | 返回桌面 |
| SELECT + X | 开关兜底鼠标，退出时恢复焦点/手柄自动模式 |
| SELECT + L / R | 音量减 / 加 5%，长按每 250 ms 重复 |
| SELECT + Y 保持 1.2 s | 启动 `DOIT-GameKit` Wi-Fi 配网热点 |

组合键会被系统消费，直到组合涉及的按键全部松开。其余按键按完整状态发送；
应用启动、恢复时每 50 ms 刷新一次状态，减少遗留按住状态。
协议在原来的 8 位后追加 X=bit8、Y=bit9、L=bit10、R=bit11，事件长度保持不变。
C SDK 使用 `PXA_CONTROLLER_X/Y/L/R`，C++ SDK 使用
`pxa::ui::controller_x/y/l/r`。需要这些键的应用应使用本工作区更新后的 SDK
重新构建；旧 SDK 的 C++ 解码器不认识新增位。

本工作区的固件使用 PXA Core 2.0，Core 1 应用包需要重新构建。例如：

```sh
tools/app.sh build plane-shooter --board doit-gamekit
```

`doit-gamekit` 的默认应用目标为 ESP32-S31 AOT。PSRAM 中的 AOT 代码与重定位
必须先写回数据缓存，再执行指令缓存屏障；板卡默认关闭会将动态 PSRAM 锁为
不可执行的预配置保护。

## 性能配置

板级默认显式固定 CPU 为 320 MHz，关闭动态调频，PSRAM 为 250 MHz。
PXA 光栅器使用 S31 的 O3 构建，运行时在核心 1、并行光栅工作线程在核心 0；
沿用 Korvo 的 `PXA_RASTER_SPAN_IRAM` 策略，将深度纹理像素热点放入 IRAM。
大块帧缓冲、纹理和 AOT 代码仍使用 PSRAM。

I2S 参考 Mosaico 开启 `CONFIG_I2S_ISR_IRAM_SAFE`，中断代码、驱动状态及
中断队列分配于 SRAM，以便缓存维护及 Flash 写入期间持续处理音频 DMA。

## 编译与首次烧录

从项目根目录执行：

```sh
source "$HOME/esp/esp-idf/esp-idf-main/export.sh"
tools/firmware.sh doit-gamekit build
tools/firmware.sh doit-gamekit flash --port /dev/ttyACM1 --baud 2000000
tools/firmware.sh doit-gamekit monitor --port /dev/ttyACM1 --baud 2000000
```

将串口替换为本板 CH343P 的实际设备。启动早期控制台为 115200，PXADB 初始化后
UART0 切为 2000000。板卡依赖锁为 `firmware/dependencies.lock.doit-gamekit`，
由 Component Manager 自动生成。

安装并启动商店等应用可直接使用开发入口：

```sh
tools/dev.sh device --board doit-gamekit store --port /dev/ttyACM1
```

应用架构从本板 `board.cmake` 的 `PXA_BOARD_TARGET` 自动读取为 ESP32-S31，
无需在开发或打包脚本中增加板名映射。此板默认 2000000 波特率；可用 `--baud` 或
`PXADB_BAUD` 覆盖串口速率，命令行参数优先。

首次烧录需要把中文字体写入 `pxa_data`。生成镜像：

```sh
tools/factory.sh image doit-gamekit
```

使用构建环境里的 esptool 将输出镜像写到 `0x510000`，例如：

```sh
python -m esptool --chip esp32s31 --port /dev/ttyACM1 --baud 2000000 \
  write-flash 0x510000 out/pxa-partitions/doit-gamekit/pxa_data-empty.bin
```

这一步会替换内部系统数据分区，供首次出厂配置使用。后续更新使用
`tools/firmware.sh doit-gamekit app-flash --port ... --baud 2000000` 保留现有数据。
5 MiB 固件分区位于 `0x10000`，其后的 10.9375 MiB 为 `pxa_data`，结束于
16 MiB flash 边界。

## 验证

```sh
bash firmware/boards/doit-gamekit/tests/test_host.sh
bash firmware/boards/doit-gamekit/tests/test_navigation.sh
bash firmware/boards/doit-gamekit/tests/test_reference.sh
tools/firmware.sh doit-gamekit build
```

本板测试覆盖同时按键、独立消抖、抖动、计时器回绕、组合键长按、音量重复与
组合键松开吞键；合成测试覆盖带填充的行宽、2×/4× 缩放、居中、负坐标裁剪、
预乘 ARGB 和系统/应用图层。新增手柄位还由 libpxa UI 事件测试以及 C/C++ SDK
解码测试覆盖。
旋转测试覆盖 90°/270°、实际 320×240 帧、非整块边缘、带填充的输入行、
原生逐行坐标、RGB565 线上字节序、输出边界和截图反向旋转。
浮层旋转测试覆盖两种方向、边界裁剪和关闭后的像素恢复；真实 LVGL 刷新
测试区分内容、仅浮层和混合更新，并检查静止时不会持续触发显示刷新。
真实 LVGL 导航测试覆盖空间焦点、普通设置行、隐藏/禁用控件、越界滚动、
滑块/开关/下拉框/键盘矩阵，以及按住 A 时打开弹窗或删除控件的取消语义。
像素渲染测试还检查紧贴父容器的主题按钮和普通行，确保焦点框四边完整。
真实共享输入法测试覆盖字键内高亮、英文首字输入、上下跨矩阵导航、数字页、
退格，以及 PXA Provider 的输入排队、候选词显示/选择和提交回填。
参考 UI 测试覆盖无底栏配置、返回、最近任务、锁屏、文件上级目录和键盘焦点。
后两个测试分别使用已构建的 `lvgl-align64` 测试库和 `product-lvgl96` 模拟器库；
依赖与构建方式见测试脚本。
模拟器显示 profile 为 `simulator/profiles/doit-gamekit.toml`，用于 320×240 布局；
它不模拟板上的 GPIO、屏幕、I2S 或卡。PXADB 支持应用安装、日志、内部存储文件传输、
单指远程输入和 RGB565/JPEG 截图。截图读取 DMA 已完成的显示快照；物理按键
接管时取消远程手势。
SD 卡可以通过系统文件管理器读写；PXADB 可列出/读取 SD 文件，上传仍遵守
现有的 `inbox`/系统目录写入策略。

本次已在 `/dev/ttyACM1` 验证：ESP32-S31 v0.0、16 MiB flash/PSRAM、
ST7789 首帧与中文桌面截图、无底栏焦点界面、1876 MiB SD 卡挂载与目录读取、
Core 2 原生 AOT 游戏及 Surface 画面、游戏音频 PCM 提交/播放链路。
软件旋转版本已核对原生 240×320 输出的启动日志，以及桌面、设置列表、滚动和
语言弹窗的已完成 DMA 帧截图；扫描撕裂的实际改善程度需观察实体屏幕。
最终应用镜像、16 MiB 首次出厂整包、校验和与验证记录位于
`out/firmware/doit-gamekit/`。出厂整包从 `0x0` 写入，会替换内部数据；日常更新
使用 `firmware.bin` 写入 `0x10000`。
屏幕模组实际朝向/色序、实体按键手感与扬声器声音尚待人工反馈。

手柄路由依据前台 PXA 根节点的 controller 事件订阅，不以游戏 Surface 是否显示判断。应用的 Canvas 菜单继续接收手柄；原生系统弹窗、输入法和最近任务始终优先导航。未订阅 controller 的界面仍用原生焦点，显式鼠标模式保持可用。
