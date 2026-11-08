# ESPP4-A

SmartArduino ESPP4-A / ESP32-P4 DevKit v1.1 的 PXA 板级适配。
硬件参数和 ST7102 初始化表来自
[SmartArduino/ESPP4-A](https://github.com/SmartArduino/ESPP4-A/tree/1e981d9f159a1afb30bd5e41b0137344ce3916a6)。

| 资源 | 接线 / 配置 |
| --- | --- |
| 主控 | ESP32-P4，400 MHz，16 MB Flash，Hex PSRAM 250 MHz |
| 显示 | ST7102，480×854，约 6×11 cm，200 DPI，RGB565，DSI 两线 440 Mbps，像素时钟 36 MHz |
| 触摸 | ST7123 兼容，I2C1 SDA49 / SCL48，7 位地址 0x55 |
| 屏幕复位 / 触摸复位 | GPIO46 / GPIO47；初始化后不再复位触摸，以免同时重置屏幕 |
| 背光 | GPIO24，高有效，LEDC PWM |
| 音频 | ES8311，I2C0 SDA28 / SCL25，7 位地址 0x18 |
| I2S0 | MCLK26 / BCLK27 / WS30 / DOUT31 / DIN29，16 kHz 双声道时隙 |
| 摄像头 | GC4023，MIPI-CSI 两线 720 Mbps，LP-I2C0 SDA9 / SCL10，地址 0x29，复位11，MCLK21 / 24 MHz |
| TF 卡 | SDMMC0 CLK43 / CMD44 / D0…D3=39…42，检测45，电源4低有效 |
| 无线 | 模组专用 `p4_wireless`，SDMMC1，GPIO14…19 / 54 由厂商驱动占用 |
| BOOT | GPIO35：短按返回，长按后松开回桌面；休眠时先唤醒 |
| 调试 | UART0 TX37 / RX38，2000000；连接板载 CH340X 的 UART-DEBUG Type-C |

显示在板级完成合成：LVGL 以 FULL 模式直接绘制两个 DSI DMA 帧缓冲，随后
合成 PXA Surface 和透明层，免去整屏 UI 复制。DMA 在帧边界切换缓冲后允许
LVGL 绘制下一帧；帧率统计和截图另等完整扫描结束，避免把提交次数当成显示帧率。
RGB565 原尺寸 Surface 使用逐行复制，2 倍／4 倍放大按源行展开并复用重复行；
ARGB 同尺寸画面按行合成，按遮挡区和透明度平面跳过不可见跨度；其他缩放
与复杂混合保留通用合成。
GameRender 的 PSRAM 分配和缓存写回按配置的 L2 缓存行大小对齐，支持本板
128 字节缓存行；帧尾保留完整缓存行，避免写回相邻对象。
DSI／DW-GDMA 中断及帧完成回调驻留 IRAM，回调上下文与静态信号量驻留
内部 DRAM，Flash 存档暂停缓存时仍能继续扫描。串口每 10 秒输出扫描次数、
超时扫描间隔及 Surface 获取失败计数；背光日志仅在状态变化时输出。
PXA Surface、系统透明层、
锁屏、物理触摸优先级和 PXADB 输入都在同一个板级显示锁下协调。
触摸支持五个独立触点，按 ST7123 原始报告的固定槽保留触点身份；抬起一指
不会移动其余触点的 ID。五个 LVGL pointer 同时供原生触屏和 PXADB 多指测试
使用，真实触摸优先接管。读取短暂失败保留触点，连续失败 150 ms 后取消全部
触点并等待松手，防止误点击或卡住的拖动；唤醒、锁屏与系统按键同样取消旧输入。
启动时 bootloader 先将 GPIO24 拉低；PXA Logo 完整扫描并稳定 100 ms 后才
开启背光，Logo 至少显示 1.2 s，再由完整桌面帧接替。首次更新启动策略需
使用 `flash` 同时更新 bootloader，后续普通应用固件更新可用 `app-flash`。
音频输出由 PXA 混音工作线程写入 I2S；麦克风 RX 仅在授权捕获资源打开期间启用。
无 SD 卡时继续启动，挂载失败不会格式化存储卡。
GC4023 使用 CSI RAW10、ISP 去马赛克与 Gamma、自动曝光及灰度世界白平衡，
通过 P4 硬件 JPEG 编码提供以下档位，原有档位 ID 保持稳定：

| ID | 拍摄尺寸 | JPEG 质量 | 用途 |
| --- | --- | --- | --- |
| 9 | 320×240 | 75 | 默认快速档，中央 4:3 裁切，原生像素预览 |
| 1 | 320×180 | 75 | 16:9 宽屏预览与录像 |
| 2 | 640×360 | 80 | 标准画质 |
| 3 | 160×90 | 60 | 减少 JPEG 数据量 |
| 4 | 480×270 | 78 | 屏幕宽度档 |
| 5 | 800×450 | 80 | 增加细节 |
| 6 | 960×540 | 78 | 中高分辨率 |
| 7 | 1280×720 | 70 | 720p |
| 8 | 1920×1080 | 60 | 1080p，直接使用原生 CSI 帧编码 |

JPEG 单帧上限仍为 256 KiB，复杂画面超过此上限时报告限制错误。高分辨率
不会提高传感器固定的 25 FPS 上限，实际录像速度还取决于编码及存储。
原生预览直接从 CSI 帧写入不可变 RGB565 Core 图像，不经过拍摄尺寸的中间
图像。已安装相机 App 的 C++ 客户端要求预览边长不超过 320；默认 320×240
档使用 App 既有的原生像素布局，从而避免宽屏预览放大时的软件重采样。
该档的预览宽度为 320 个物理像素；16:9 档继续按屏幕宽度显示。
320×240、320×180 档的预览图像同时作为 JPEG 输入 DMA 的只读缓冲；
1080p 直接读取受持有保护的 CSI 帧，其余档位只为 JPEG 保留一次缩放。
编码为独立图像后提前归还 CSI 帧；Flash 写入等背压后丢弃超过 80 ms 的旧帧，
等待当前画面。录像的 JPEG 和预览复用同一帧。相机硬件仅在获得
`camera.jpeg` 权限后由相机工作线程启动，关闭后停止采集并释放 DMA 缓冲、
ISP、JPEG、LP-I2C 和 MCLK。摄像头不重新配置或释放显示共享的 MIPI LDO3。
两个固定 1080p CSI 缓冲与当前档位的编码缓冲为有界板级 DMA 内存；发布到
Core 的图像另受资源预算约束。CSI 和显示的 DMA 中断保持缓存暂停期间可用。
`camera.lf` 补齐当前 IDF 未纳入 IRAM 的 DW-GDMA 传输配置函数，确保相册
写入 Flash 时 CSI 中断可以继续交接缓冲；ISP 使用独立 160 MHz 时钟源。
普通内存 JPEG 通过板级完整图像解码入口复用 `esp_lv_decoder`，避免把其仅供
分片格式使用的区域回调应用于相册 JPEG。超过 640 像素边长时使用现有 JPEG
解码器的缩放输出，将显示工作缓冲限制为至多 512×512，保留磁盘文件的原始
分辨率。图像分配继续受既有预算约束。

TF 卡在无线模组完成共享 SDMMC 初始化后挂载，使用单线 5 MHz 保守配置。
挂载点为 `/pxa/sdcard`，使用 UTF-8 长文件名，支持直接安装相机应用，
其私有相册随应用保存在同一卷。相机应用支持 JPEG 拍照、MJPEG/Opus MKV
录像及相册回放；麦克风需要另行取得 `audio.capture/microphone` 权限。
本板每应用私有文件总配额为 16 MiB，照片和录像共享配额，且仍受卡片剩余
空间限制。挂载失败时不会格式化卡片，移除卡片前应先停止录像并退出相机。

## 构建与接入

使用提供 ESP32-P4 v3 支持的 ESP-IDF 6.1/6.2 环境，以及对应的 RISC-V 工具链。
板级 defaults 选择 newlib，以匹配厂商无线二进制 ABI。

```sh
tools/firmware.sh espp4-a build
tools/firmware.sh espp4-a flash --port /dev/ttyUSB1 --baud 2000000
tools/firmware.sh espp4-a monitor --port /dev/ttyUSB1 --baud 2000000
python3 tools/pxadb/pxadb.py info --port /dev/ttyUSB1 --baud 2000000
```

CH340X 的 RTS / DTR 接到 EN / BOOT。PXADB 客户端在打开桥接串口前释放
这两根控制线，并给打开串口时可能发生的复位预留启动时间。多步交互验证使用
同一个 PXADB 连接，例如 `pxadb run <scenario>` 或 GUI 的持续连接，避免逐条
命令重新打开串口造成应用重启。
控制台与 PXADB 共用 UART 驱动和同步 TX，避免轮询控制台日志插入协议帧。

`factory` 应用分区为 7 MiB，`pxa_data` 从 `0x710000` 开始，占用剩余
`0x8f0000` 字节。固件烧录不覆盖该数据分区；已有数据挂载失败时保留原内容。
首次出厂接入可使用通用工厂配置生成带系统字体的 LittleFS 镜像：

```sh
tools/factory.sh image espp4-a --profile factory/profiles/generic.toml \
  --output local/artifacts/espp4-a/factory/pxa_data.bin
```

为新设备烧录此镜像到 `0x710000` 后重启。对已有设备，先备份数据，按
项目的 PXA 应用交付流程迁移；不要把工厂镜像当作日常应用安装方式。
后续使用 PXADB 安装独立的 `.pxa` 应用。设备目标是 `esp32-p4`，AOT
产物必须匹配此目标；不能安装 S3 或 S31 的专用机器码。

## 无线与许可证

无线驱动随源码保存在 `vendor/p4_wireless/`，固定于上述上游提交并保持原样。
它支持 STA 或 SoftAP，不支持 APSTA。板级链接包装将共享配网页面申请的
APSTA 转为 AP；配网控制器先停止 STA，再启动 AP，因此不会并行运行两种模式。
日常联网可直接在系统 Wi-Fi 设置中扫描和连接。
`espp4_a_sdio.cc` 在 SDMMC 公共 API 边界为不满足当前 IDF DMA 对齐要求的
无线缓冲提供内部 DMA 暂存，保留原始地址、传输长度及错误返回，不修改厂商库。
板级 RTOS 包装将无线总线工作线程的最小栈设为 6 KiB，满足当前 IDF 的
SDMMC 调用深度。启动时暂留 40 KiB 连续 SRAM，串口服务初始化后交还给
PXA 运行时，避免外围设备分配将运行时线程所需的连续栈空间碎片化。

`p4_wireless` 采用厂商专用许可，只允许在 SmartArduino 或其授权分销商的
ESPP4-A 模组上使用和随产品原样分发。必须保留
`vendor/p4_wireless/LICENSE`，不得修改二进制或绕过模组授权。
ST7102 初始化表采用上游 Apache-2.0 许可，完整许可证保存在
`vendor/LICENSE.upstream`。newlib 和 GCC 运行库的归属声明保存在
`vendor/runtime-licenses/`；自动许可门禁固定审查通过的目标归档和许可证哈希。

GC4023 的 ESP32 适配保留 SmartArduino 的 Apache-2.0 许可，寄存器及增益表
核对自 OpenHisilicon 的 MIT 传感器库，保留 Shenshu 原始版权。OpenHisilicon
对传感器库和内核模块分别声明 MIT 与 GPL，固件只使用前者。固定来源、完整
许可及修改说明见 `vendor/gc4023/NOTICE`，不以无许可的 HiSpark 副本作为授权依据。

Surface-ready notifications wake the display task; new frames refresh through the normal LVGL trusted-UI and DMA path. Integer 2× RGB565 composition expands aligned pixel pairs with 32-bit stores and reuses identical source rows. Clipped, unaligned and alpha-bearing rows preserve the same composition rules.
