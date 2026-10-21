# Surface 尺寸限制与 Korvo 实机验证

2026-10-20：ESP Host 不再将 Surface 限制为 320×240，也不要求其尺寸小于面板。当前桌面产品模拟器及旧 C Host 模板同步使用完整协议范围。

## 准入规则

- 宽、高仍使用协议中的 `uint16_t`，有效值 1～65535。
- 单帧字节数必须能由协议的 `uint32_t` 表示，乘法先用 64 位检查；保留步长、映射范围及实际分配失败检查。
- `max_width`、`max_height`、`max_frame_bytes` 配置分别为 `UINT16_MAX`、`UINT16_MAX`、`UINT32_MAX`。配置本身不预留任何帧缓冲。
- 每组件 Surface 数量和双/三缓冲约束保持原有资源策略。内存不足时仍返回资源错误。
- Surface 创建与屏幕呈现是不同步骤：大于屏幕的画布可创建，实际可见区域由 presenter 裁剪；本次大尺寸实机探针验证分配/释放，没有将所有极端尺寸提交给 LCD。

修改位置：

- `firmware/components/pxa/src/services/pxa_esp_services.c`
- `deps/pxa-system/simulator/desktop/product_runner.c`
- `tools/host-sdk/simulator/pxa_c_host.c`
- `deps/pxa-system/libpxa/tests/surface_test.c`

桌面产品模拟器还取消了“大于面板即拒绝”的后端条件，并修正大画布居中计算的无符号下溢。旧 C Host 将可见图像限制在视口，保留原 Surface 的完整步长，避免 LVGL 图像头的窄字段截断大步长；该模板通过语法编译检查，完整运行验证使用当前产品模拟器。

## Korvo 实测

设备：`esp32s31-korvo-1`，800×480，串号 `30:ed:a0:f3:de:10`；PXADB `/dev/ttyUSB0`，2,000,000 baud。

临时签名探针逐个创建 **Host 实际分配的 RGB565 双缓冲**，随后关闭句柄：

| 尺寸 | 单帧字节数 | 结果 |
|---|---:|---|
| 800×480 | 768,000 | 创建、释放成功 |
| 1024×768 | 1,572,864 | 创建、释放成功 |
| 65535×1 | 131,070 | 创建、释放成功 |
| 1×65535 | 131,070 | 创建、释放成功 |
| 65535×65535 | 超过 `uint32_t` | `LIMIT_EXCEEDED (-16)`，分配前拒绝 |

原始证据：[结果](nes-korvo-20261020/surface-probe-results.json)、[串口日志](nes-korvo-20261020/device-surface-probe.log)、[探针结束内存状态](nes-korvo-20261020/device-surface-probe.json)。

该次探针固件 BIN SHA-256：`22d5b3d807be87bb666d3f619be90c3534ed385a0174144002172cbfa308ccbd`。后续 NES 最近邻放大修复未修改上述准入逻辑。

Core Surface 回归和 ESP Host 原生回归均通过；当前桌面产品模拟器构建、NES 400×240 映射 Surface 运行通过。NES 已在 Korvo 创建 400×240 Guest 映射双缓冲并运行，验证了原 320 像素限制导致的启动失败已解决。

## 数据保留

刷写只覆盖 `0x10000` 应用分区，原固件保存在本地 `local/nes-korvo-20261020/korvo-original-app.bin`。没有擦除或格式化文件系统。首次固件更新前后的实际目录清单一致，见[目录检查](nes-korvo-20261020/filesystem-inventory-check.json)。临时 Surface 探针验证后已卸载，NES 与原拼音输入法保留。
