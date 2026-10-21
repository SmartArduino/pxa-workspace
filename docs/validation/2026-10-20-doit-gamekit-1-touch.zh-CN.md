# DOIT GameKit 1 屏幕与触摸适配

## 硬件依据

用户提供 `CL32CK305-18A_CTP.rar`。包内规格书第 3 页列出 ILI9341V、FT6336U、
240×320 像素和 48.60×64.80 mm 有效区域，但使用其初始化表后，用户反馈
颜色、方向和触摸异常。随后提供工作的参考工程
`/home/lucinhu/Documents/workspace/esp32/game_-console`。

参考工程 `components/lcd/st7789.c` 使用 **ST7789V TN** 初始化，与前述规格书
的 IPS 初始化表不同。本次最终实现以该参考工程为准，完整复用初始化指令、
参数和延时，采用 `COLMOD=0x05`、`MADCTL=0x60`、RGB、不反色、60 MHz SPI。
320×240 横屏帧按行输出 RGB565 高字节、低字节，不做软件旋转。
初始化逐项比对结果和参考源文件校验和见
[reference-audit.json](doit-gamekit-1-touch-20261020/reference-audit.json)。
规格书副本保存在板级 `docs/`，原始资料校验和见
[sources.json](doit-gamekit-1-touch-20261020/sources.json)。

## 实机观测

设备端口 `/dev/ttyACM1`，CH343P，PXADB 为 2000000 波特率。

| 项目 | 结果 |
| --- | --- |
| 芯片 | ESP32-S31 v0.0，16 MiB Flash / PSRAM |
| PSRAM | 250 MHz，启动内存测试通过 |
| FT6336U | 地址 0x38，芯片 ID 0x64，固件 0x13 |
| 触摸连接 | SDA=13、SCL=14、INT=15、RST=16，100 kHz |
| SD | 1876 MiB，SDMMC slot 0，4 位、20 MHz |
| 显示完成帧 | 320×240，PXADB JPEG 截图成功 |
| 系统 | PXA ready，桌面可见内置设置入口 |

触摸沿用参考工程的坐标约定：传感器原生 240×320，交换 X/Y 后反转 Y，
即 `(x,y) → (y,239-x)`。显示和触摸均以参考工程的横屏方向为准。

参考版本已在 `/dev/ttyACM1` 完成应用烧录及设备端哈希校验。重启日志确认
`COLMOD=0x05 MADCTL=0x60, inversion=off`，输出 320×240、无软件旋转；
FT6336U 初始化成功，并收到实体触摸 DOWN/UP。PXADB 显示 `pxa=ready`，
运行中的 ELF 哈希与本次镜像一致，完成帧截图来源为
`st7789-reference-landscape-completed`。固件校验和和详细结果见
[manifest.json](doit-gamekit-1-touch-20261020/manifest.json)。

## 实现与测试

- 按参考工程复位 FT6336U，不额外写入工作模式寄存器，每 10 ms 读取状态及两个记录。
- 解析 DOWN/CONTACT/UP，按 tracking ID 保持两个独立 LVGL 接触槽位。
- 保留边沿、只合并移动；系统切页及连续读失败取消旧手势，唤醒序列等待松开。
- 实体触摸接管时取消 PXADB 指针/手柄注入，按键可恢复焦点导航。
- 主机测试覆盖两个安装方向的全部 76,800 个原生像素，检查参考坐标映射；
  还覆盖非法长度/数量/坐标、重复 ID、释放、换序和槽位复用。
- 输出测试覆盖 RGB565 原色字节、横屏行顺序、1/2/4 倍缩放、源缓冲跨度、
  截屏回读、裁剪叠加层及边界哨兵，三个测试均通过 ASan/UBSan。
- 完整构建、codec memory audit 和应用分区容量检查通过。

测试、构建、串口及截图位于
[验证目录](doit-gamekit-1-touch-20261020/)。截图取自完成输出的缓冲区；
实体屏幕颜色、方向及触摸四角落点需要人工核对。未将芯片支持的两个记录等同于
模组已实测双指能力，模组规格书标注为「单点加手势」。

## 烧录与恢复

原机分区表仅包含 NVS、PHY 和 1 MiB 应用分区。烧录前备份了其全部已用区域
`0x000000–0x10ffff`，文件为
`out/firmware/doit-gamekit-1/original-used-partitions-backup.bin`，另存 SHA-256。
首次写入新启动程序、分区表、应用及含字体的 LittleFS 镜像，并通过设备端校验。
颜色/方向修正只更新 `0x10000` 应用分区。

日常固件镜像保存在 `out/firmware/doit-gamekit-1/firmware.bin`。
