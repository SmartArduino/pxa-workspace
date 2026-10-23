# EPDiy V7 40-pin 墨水屏

适配指定的 `epdiy_v7_40pin_factory_waveform_normalized` 参考工程。
这是该工程的定制 40-pin 接线版本，不能直接视为所有 EPDiy V7 和所有面板的通用配置。

| 项目 | 配置 |
| --- | --- |
| 芯片 / 存储 | ESP32-S3、16MB Flash、8MB Octal PSRAM |
| 屏幕原生分辨率 | 1216 × 684，16 位并行总线 |
| 默认显示方向 | 684 × 1216 竖屏，按实物安装方向额外旋转 180°，300 DPI |
| 波形 | 参考工程自带 ED047TC2，DU / GC16 / GL16 |
| VCOM | 默认 -1.20V，按实际屏幕标签修改 |
| 显示时钟 | 参考配置 bus_speed=24，16 位总线实际 CKH=12MHz |
| 调试 / PXADB | UART0，TX=GPIO43、RX=GPIO44，921600 baud（ROM 启动日志仍为 115200） |
| 输入 | GPIO0 BOOT 短按唤醒 / 返回桌面；PXADB 单指点击、Home、Back |
| 数据分区 | `pxa_data`，LittleFS，挂载 `/pxa` |

参考工程中的 `ED060XC3` 名称已被重用于上述分辨率及波形，本适配用独立
`epdiy_v7_panel` 定义保留其实际参数。没有参考依据的触控、音频、SD 卡和电池采样
未声明为硬件能力。需要触控时，应补充实际控制器型号、地址与引脚。

## 构建

使用 **ESP-IDF 5.5.4**。板级 manifest 限定 `>=5.5.4,<6.0`，因为参考 LCD_CAM
输出直接使用该版本系列的 GDMA / RMT HAL；没有声称兼容 IDF 6.x。

在项目根目录运行：

```sh
source /path/to/esp-idf-v5.5.4/export.sh
tools/firmware.sh epdiy-v7 build
tools/firmware.sh epdiy-v7 menuconfig
tools/firmware.sh epdiy-v7 flash --port /dev/ttyUSB1 --baud 921600
python3 tools/pxadb/pxadb.py info --port /dev/ttyUSB1 --baud 921600
./tools/pxadb-gui.sh --port /dev/ttyUSB1 --baud 921600
```

默认构建目录为 `build/firmware/epdiy-v7/`。板级依赖已独立锁定在
`firmware/dependencies.lock.epdiy-v7`，只选择此板时才会参与解析。
烧录参数以构建生成的 `flash_args` 为准：ROM 启动头使用 DIO，bootloader 随后
切换到 QIO；直接将启动头覆盖为 QIO 会导致 ROM 反复重启。

`EPDiy V7 40-pin` 菜单可调整方向、VCOM、物理刷新间隔、清屏周期、DU、照片抖动
及前光。常规固件构建不生成或烧录 LittleFS 数据；首次使用需按项目的
[PXA 应用交付说明](../../../docs/zh-CN/pxa-app-delivery.md)准备数据分区、系统字体与应用。
分区挂载失败不会自动格式化。

本机 CH340 曾因 Linux USB runtime PM 每约 10 秒断开再枚举。当前 PXADB 客户端
在连接期间自动只读保活。对于 esptool 等其他串口命令，遇到同样情况可包一层
只读保活（命令结束即释放，不改系统电源策略）：

```sh
python3 firmware/boards/epdiy-v7/tools/with-usb-awake.py --port /dev/ttyUSB1 -- \
  python3 tools/pxadb/pxadb.py info --port /dev/ttyUSB1 --baud 921600
```

## 墨水屏策略

- LVGL 用 16 行 RGB565 小缓冲渲染，完成应用画面、透明层和系统浮层合成后转换为
  4bpp 灰度。期望帧、发送帧、已显示帧和差分缓冲合计约 1.98MiB，均在 PSRAM。
- 波形由独立任务执行，不持有 LVGL 锁或应用 Surface 租约。完整 LVGL 刷新结束后才
  取快照；积压时合并到最新画面，不排队逐帧刷新。
- 默认最短物理刷新间隔 350ms；实际间隔还取决于波形耗时。转换后完全相同的画面
  不触发高压、不刷屏；静止画面不进行周期轮询刷新。
- 仅在变化像素的前后状态都为纯黑或纯白时选择 DU；含灰阶的变化使用 GL16。
  变化像素达到全屏 40% 时使用 GC16。小区域比较只扫描必要的差分行；物理输出
  保持全宽扫描，用行 / 列掩码避免驱动未变化像素。
- 冷启动、失败恢复，以及连续 20 次局部刷新后的下一次画面变化执行物理清屏再
  GC16。清屏会闪烁，用于消除累积残影。相同画面不会为了计数强制闪屏。
- 每次刷新后关闭 TPS65185 高压。电源 PG 失败不提交“已显示”状态；输出失败
  保留旧状态并延迟重试，下一次先清屏。
- 读取 TPS65185 温度选择波形。保留原始 LUT / 相位数据，修正原文件温区指针与
  实际保留的 `_5` 至 `_11` 数据不匹配的问题，使用 15–38°C 的七组温区；超界
  选择最近一组，不能据此保证超出面板工作温度时的显示质量。
- 默认浅色石墨主题、高对比度、关闭系统动画和壁纸；可选 4×4 有序抖动用于照片。
  抖动锚定绝对坐标，局部重绘不会因 tile 边界变化而产生噪声。
- S3 向量差分路径保留 1KB LUT，并修正行队列指针表过度分配；相对参考工程共节省
  约 100KB 内部 SRAM，显示线队列和 DMA 仍使用内部内存。
- 每相位结束后清空行队列中的尾部填充行，并为 DMA 启动预留队列容量，避免下一
  相位在启动输出前双核同时等队列空间。输入单独以 10ms 轮询，不受 100ms 绘制
  周期限制，短按不会因降低画面刷新率而丢失。
- mbedTLS 分配到 PSRAM，给 LCD DMA、行队列和任务栈保留内部 SRAM；HTTPS 保留
  系统证书校验。只启用 PSRAM 而沿用 IDF 的 TLS 内部内存默认值会在联网时失败。
- PXADB 提供原生灰度预览：直接从已完成的面板帧编码 gray4 RLE，保留 684×1216
  分辨率及全部 16 级灰度。临时缓冲上限约 0.79MiB，不需要完整 RGB565 和 JPEG
  输出缓冲；GUI 检测 `screenshot-gray4-rle` 后自动使用该格式。

```sh
python3 tools/pxadb/pxadb.py screenshot --port /dev/ttyUSB1 --baud 921600 \
  --gray4 --after-present screenshot.png
```

锁屏保持墨水屏上的最后画面，关闭前光并暂停物理刷新；它不代表整机深度睡眠。
BOOT 或远程点击可唤醒，解锁仍由系统锁屏界面处理。

## 引脚与前光

| 信号 | GPIO |
| --- | --- |
| I2C SDA / SCL / INT | 39 / 40 / 38 |
| PCA9535 / TPS65185 地址 | 0x20 / 0x68 |
| D0…D7 | 5, 6, 7, 15, 16, 17, 18, 8 |
| D8…D15 | 9, 10, 11, 12, 13, 14, 21, 47 |
| CKH / CKV / STH / LEH / STV | 4 / 48 / 41 / 42 / 45 |
| 可选暖光 / 冷光 PWM | 3 / 46，2kHz，10 位 |

前光默认关闭硬件支持，因为它是参考工程的外接接线。确认接线后启用
`CONFIG_PXA_EPDIY_FRONTLIGHT`；系统亮度同时控制两路，可调整暖光占比，idle dim
不修改保存的亮度设置。

## 验证

```sh
bash firmware/boards/epdiy-v7/tests/test_host.sh
```

主机测试使用 ASan / UBSan，覆盖灰度、像素 nibble 顺序、旋转、脏区域边界、DU 的
灰阶保护、清屏策略，以及横竖屏的 Surface 缩放、stride、ARGB 和可信浮层。
VCOM 电压、残影的光学评价及可选前光仍需实物测量。

驱动来源与本地修改见 [vendor/epdiy/UPSTREAM.md](vendor/epdiy/UPSTREAM.md)。
