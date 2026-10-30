# SenseCAP Watcher

本板型使用 ESP32-S3、32 MiB Flash、412×412 SPD2010 圆屏与触摸、旋钮和
按压键、PCA9555 电源管理、ADC 电池测量、ES8311 播放、ES7243/ES7243E
麦克风和 PCF8563 实时时钟。硬件定义参考
[Seeed 官方 BSP](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/tree/8e37f7ccd3d836bb99f483e0f6e74e8252406644/components/sensecap-watcher)。

```sh
tools/firmware.sh sensecap-watcher build
tools/firmware.sh sensecap-watcher app-flash --port /dev/serial/by-id/<Watcher串口> --baud 2000000
```

使用实际 USB 标识发现串口，不将 `/dev/ttyACM` 编号作为板型身份。烧录前读取
设备 INFO 和分区表；只有分区布局匹配时才能使用应用区更新。

## SD 卡

卡槽使用 SPI2：CLK=GPIO4、MOSI=GPIO5、MISO=GPIO6、CS=GPIO46。
PCA9555 P0.4 为低电平有效的卡检测，P1.0 控制供电。LCD 独立使用 SPI3。
启动时存在 FAT 卡则挂载到 `/pxa/sdcard`，系统文件管理器和 PXADB 使用
`/sdcard`，应用通过标准 Storage/FS SDK 枚举与授权访问。支持 UTF-8 长文件名。

无卡时跳过 SD 挂载并继续启动；卡损坏或文件系统不支持时不会格式化。
当前按启动时卡状态挂载，插拔请先关机；尚未实现运行中的自动卸载和重新挂载。
SPI2 也连接 Himax AI 芯片。板子统一初始化该总线，SD 和视觉驱动分别管理
片选；无卡仍保留视觉通路，SD 挂载失败不会释放共享总线。应用不能访问板内
控制引脚，也不能重新初始化或释放总线。

## 音频与时钟

播放走标准 Audio 服务。麦克风按 I²C 地址识别 ES7243（0x13）或 ES7243E
（0x14），读取右声道单声道 I²S，以 16 kHz/20 ms Opus 包交给标准 Audio
捕获服务。编码与有界 PCM 读取运行在工作线程，关闭捕获后停止 ADC；关机前
等待捕获工作线程退出。没有识别到 ADC 时保持播放可用，不假报录音能力。

PCF8563（0x51）是系统时钟的保留 UTC 来源，显示时区由系统处理。低电压、
停振、无效 BCD、无效日期或 I²C 失败时回退到系统/NVS 时钟；网络校时或手动
设时后的值在系统保存时写回 RTC，不周期性写 Flash。日历支持 2000–2099，
采用世纪位 0 对应 2000 年代的约定。RTC 的闹钟、定时输出没有映射为传感器。

## 接口边界

电池/充电、输入、网络和时间使用已有系统接口，SD 使用标准 Storage/FS，
声音使用标准 Audio。本板没有已接入的加速度计或陀螺仪，不提供虚构的运动数据。
RGB 灯（GPIO40）通过仓库 `org.doit.peripherals` 扩展枚举为
`indicator.rgb`，种类 RGB=5、模式 RGB=6，写入三个 RGB 字节。应用必须完成
UUID/版本协商，取得该精确 scope 的权限后打开；退出、进入后台或撤权时由
工作线程清灯，清理完成前不会把灯交给下一个应用。

J5 Grove 的 SCL=GPIO48、SDA=GPIO47，与系统 I²C0 共用总线，按
[Seeed 接口说明](https://wiki.seeedstudio.com/extending_grove_with_mcp/)接线。
扩展枚举 `grove.i2c`；应用申请同名 scope 后，以 10–400 kHz 和目标 7 位
地址打开。0x13、0x14、0x18、0x21、0x30、0x51 为板内保留地址，拒绝访问。
同一地址只允许一个持有者；最多四个原生连接，每次发送/接收各不超过 256
字节，组合操作使用 repeated start。初始化、传输和卸载在工作线程执行，
后台或撤权取消后续结果，但已开始的外设写入不会回滚。总线与 3.3 V 供电
由板子管理，应用不能更改引脚或关闭电源；具体 Grove 模块需应用自己的驱动。

## Himax 摄像头与推理

沿用 `org.doit.peripherals` 0.3 扩展，枚举只读 `camera.jpeg` 和
`vision.inference`，通过标准 Core READ 读取有界帧；使用
`<pxa-ext/vision.hpp>` 的 `FrameReader`，不要把它们当作标准 Sensor。
SDK 示例位于 `extensions/peripherals/examples/camera`。摄像头和推理分别
申请自己的精确 scope，权限界面明确显示用途；后台、退出和撤权关闭会话。

ESP 通过 SPI2（CLK4、MOSI5、MISO6、CS21）读取现有 Himax 固件，按需发送
单次 SAMPLE 或 INVOKE，不向应用开放任意 AT 命令、复位、模型安装或刷写。
图像规格由 Himax 当前传感器配置决定；原厂 240×240 配置返回 JPEG。每帧
JPEG 最多 256 KiB，推理 JSON 最多 8 KiB，单次命令截止 12 秒。收到的 JSON
是 SSCMA INVOKE 事件封装，实际目标类别和坐标语义由模型定义。没有可运行
模型时返回错误或超时，不生成伪造结果；需要模型的应用应提示用户检查模块。

SD 和 Himax 共用 SPI2、LCD 使用 SPI3。驱动只释放自己的 SPI device，不能
释放 SD 总线；轮询就绪状态，不另建一套 PCA9555 驱动改变电源影子状态。
Seeed Apache-2.0 SPI 子集的来源、许可证和本地改动保存在 `vendor/sscma-spi/`。
非标准合同按
[工作区扩展约定](../../../docs/zh-CN/pxa-extensions.md)管理。

```sh
bash firmware/boards/sensecap-watcher/tests/test_host.sh
```

该测试覆盖显示合成/IO 和 RTC 日历边界。SD 实卡读写、麦克风电气路由和掉电
计时仍需对应实物验证；无卡启动通过不等于完成实卡读写验证。
