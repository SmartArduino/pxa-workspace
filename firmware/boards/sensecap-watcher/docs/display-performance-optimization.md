# SenseCAP Watcher 显示与触摸优化记录

本文记录 Watcher 板（ESP32-S3 + SPD2010 412×412 QSPI + PCA9555 + SPD2010 触摸）
从"能显示但花屏/白屏/卡顿"到"无花屏、通知栏拖动 23~36 FPS"的完整过程，
包含每一步的现象、根因、改动和实测数据，便于后续回归时对照。

若要快速了解采用了哪些优化、实现入口及迁移约束，先读[游戏优化方法与复用参考](game-pipeline-optimization-reference.zh-CN.md)。
下文同时保留历史 UI 调优，不能把历史参数或通知栏帧率当作当前游戏配置与结果。

**2026-10-14 更新：** 下文 23～36 FPS 是历史通知栏场景，不能代表游戏帧率。
首轮板级修复使原生 412×412 Jump Jump 3D 从约 12.2 FPS 提升至 18.6 FPS，并恢复音频输出。
本轮在正确画面下实测静止 **34.33 / 34.48 FPS**、成功跳跃 **31.60 / 31.92 FPS**，画质及玩法不变。
首次冷启动额外复核为静止 **33.73 / 33.57**、跳跃 **29.89 / 29.73 FPS**，不能把较快的 UI 后结果代表所有启动状态。
相同 UI 预热后四轮最坏稳定 SRAM 519496 → 511624 B、PSRAM 3372160 → 3334176 B；
游戏峰值 523856 / 3432272 → 515984 / 3392716 B，初始化峰值也下降。
SRAM heap 总容量比基线少 80 B，按固定参考容量补回后仍全部下降。帧率来自不同帧 DMA 完成间隔。
成功跳跃输入到关联显示完成 P95 109.7 / 111.1 → 78 / 80 ms；Guest 分发并非每轮改善，未做物理触摸 / 光学测量。
两次冷启动 UI 验证完成 25 + 46 次 SHA 校验截图、3 次及 10 次启动；SRAM 峰值 518420 B，PSRAM 最坏 4982044 B，
低于匹配基线 533356 / 4985628 B。UI 后跳跃均值超过 30，首次冷启动约 29.8 FPS，尚非逐帧锁定。

此前原生 35 / 39 FPS 数据全部撤回：用户实拍发现重复横带，根因是临时 Surface 漏设 visible，实际跳过游戏像素转换。
现在原生分块直接调用独立的原始 RGB565 转换，不再伪造 Surface 描述符。生产转换器到 transport 的整帧 339488 B 回归通过，
用户物理屏幕确认修复正常。Host Surface 截图绕过 SRAM 送屏小块，仅能验证逻辑画面，不能单独证明显示正确。
SPD2010 的左边界和排他右边界须按四像素对齐；原生 `[0,412)`、LVGL rounder 及送屏检查均覆盖该要求。

当前采用单 LVGL draw unit、两个 200 行 PSRAM UI 缓冲、两个十二行 SRAM DMA 块（共 19776 B）。
原生游戏持有已有 Surface lease，融合字节交换与分块拷贝，下一帧光栅化与 DMA 重叠；透明层及缩放保留镜像合成路径。
大 Raster 资源直接使用既有 PSRAM 后备池，Watcher 性能构建的 Raster 使用 O3，16 字节对齐的转换使用局部 S3 SIMD，其他数据保留标量路径；设备数据是组合收益。
SPI3 保持 40 MHz；SPI2 / SPI3 直接 PSRAM DMA 都曾下溢，因此未采用，也未修改 IDF 私有驱动。
音频无声曾由连续 SRAM 不足导致：混音器需要 8592 B，而旧最大空闲块只有 8192 B。
单绘制单元释放了两条 32 KiB 工作线程栈，音频会话和非静音 I2S 输出、解码零 underrun 已验证；未作声学测量。

首轮记录：`pxa-apps/jump-jump-3d/docs/sensecap-watcher-20261014.zh-CN.md`；
本轮完整对比、撤回原因、真实局部峰值和限制：`pxa-apps/jump-jump-3d/docs/sensecap-watcher-pipeline-20261014.zh-CN.md`。

参考对象：同仓库的 pai-touch 板（ST7789 SPI + CST826 + **TE 锁帧**），
其 `DISPLAY_OPTIMIZATION.md` 记录了三段流水线方案；Watcher 的硬件条件不同，
不能照搬，差异见文末。

---

## 1. 硬件与软件栈

| 项目 | 参数 |
| --- | --- |
| SoC | ESP32-S3，240 MHz，8 MB Octal PSRAM @80 MHz，32 MB flash |
| 显示 | SPD2010，412×412，QSPI（SPI3），无 TE 引脚（原理图已核对，TE 未接 ESP32） |
| 触摸 | SPD2010 触摸，I2C1（GPIO38/39，400 kHz），INT 经 PCA9555 P0.5，扩展器 INT=GPIO2 |
| 电源/输入 | PCA9555（I2C0）、滚轮音量、电源键、ES8311 音频、电池 ADC |
| 图形库 | 当前 LVGL 9.6.0~1 + esp_lvgl_port 2.9；历史记录为 LVGL 9.5 |
| 运行时 | PXA/WAMR guest 系统 + reference UI（`deps/pxa-system`） |

---

## 2. 最终显示配置（板级）

`src/watcher_hardware.cc`：

- QSPI 时钟 **40 MHz**（`WATCHER_LCD_PIXEL_CLK_HZ`），总线单次上限 9888 B
  （`kLcdChunkBytes`）；正常使用两个 9888 B 的内部 DMA 块，每块十二行，原生全帧尾部四行；
- LVGL 局部渲染模式：**全宽 200 行 PSRAM 双缓冲**（412×200×2B×2 ≈ 322 KiB），
  `LV_COLOR_FORMAT_RGB565_SWAPPED` + `swap_bytes = false`（免 CPU 字节交换）；
- **1 个软件 draw unit**（`LV_DRAW_SW_DRAW_UNIT_CNT=1`），UI 在既有 owner 任务绘制，
  不再额外分配两条 32 KiB 工作线程栈；
- 游戏呈现任务在 core 1、优先级 4，光栅化在 LVGL 锁之外执行；呈现阶段同时持有端口锁和 LVGL 全局锁。
  RGB565 拷贝内核使用 `-O3`、对齐像素对读写。完整、无透明层的原生 Host Surface 持有 lease 至 DMA 完成，
  不复制整帧镜像；缩放、Guest 映射及透明合成沿用已有镜像，DMA 完成前不可修改；
- 独立的 **刷屏任务**（`watcher_flush`，优先级 4，栈 3 KB，队列 1）：
  LVGL 的 flush 回调只入队；板级 `watcher_lcd_io` 使用公开 SPI API，在刷屏任务中将像素转换/复制到两个固定 DMA 块。
  重用块之前等待该块真实完成，尾块结果也检查；正常像素路径无临时 DMA 分配或驱动弹跳拷贝。
  游戏入队后返回，允许下一帧 Raster 与扫描输出重叠；旧信号量和 lease 保证截图、UI 切换及写入等待完成。
  LVGL `flush_wait_cb` 用原子完成标志和 `vTaskDelay(1)` 等待；刷屏任务只发布完成，
  由 LVGL owner 自身结束 flushing 状态。成功和失败均释放等待，避免跨任务改 LVGL 状态或忙等饿死 idle。
  冷启动重复打开曾在默认忙等中触发 watchdog，此回调经过同流程冷启动与反复启动回归。
- PXADB 控制栈由 20 KiB 调至 18 KiB，其他板维持默认；部署和截图均实测高水位。
  截图 SHA-256 用 1 KiB 分段更新，避免 PSA 一次复制整幅 JPEG / RGB565 到内部 SRAM。
  JPEG 输出保留原先两张原始帧容量；小输出重试在真实供应商库产生堆损坏，已撤回。
  Watcher 可选配置把截图线程的 ≥4096 B 编码工作区优先放到 PSRAM；无常驻缓存。
  由于 esp_new_jpeg 1.0.2 没有公开的逐实例分配器，两个私有分配入口被隔离适配，库升级必须复测；其他板默认关闭。

`sdkconfig.defaults` 关键项：

```
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=6
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=24
CONFIG_LV_DEF_REFR_PERIOD=16
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1
CONFIG_LV_DRAW_THREAD_PRIO=4
CONFIG_LV_DRAW_THREAD_STACK_SIZE=32768
CONFIG_LV_CACHE_DEF_SIZE=1048576
CONFIG_LV_IMAGE_HEADER_CACHE_DEF_CNT=32
CONFIG_LV_USE_SYSMON=y
CONFIG_LV_USE_PERF_MONITOR=y
CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y
CONFIG_LV_USE_LOG=y
CONFIG_LV_LOG_LEVEL_USER=y
```

---

## 3. 性能测量方法

LVGL 性能监视器（log 模式）每 300 ms 输出一行：

```
I (xxxxx) LVGL: [User] lv_sysmon: 26 FPS (refr_cnt: 8 | redraw_cnt: 8),
    refr 36ms (render 35ms | flush 0ms), CPU 100%
```

| 字段 | 含义 |
| --- | --- |
| `refr` | 平均单帧刷新耗时 = render + flush |
| `render` | LVGL 渲染 + 等待上一次传输完成的时间（瓶颈通常在它） |
| `flush` | flush 回调耗时（刷屏任务后应接近 0） |
| `refr_cnt`/`redraw_cnt` | 300 ms 内刷新次数 / 实际重绘次数 |

诊断日志关键字：

| 日志 | 含义 |
| --- | --- |
| `spi_master: Failed to allocate priv TX buffer` | 内部 RAM 不足，SPI 弹跳缓冲分配失败（白屏根因） |
| `WatcherHw: flush failed (...)` | flush 提交失败（已兜底释放 LVGL） |
| `WatcherHw: Heap after bring-up: internal A/B free (min C), PSRAM D/E free` | 启动后堆余量 |
| `WatcherHw: touch: released (lift record / no reports)` | 触摸释放及原因 |

---

## 4. 历史 UI 优化时间线（2026-10-14 之前）

### 4.1 花屏（刷新区域）—— direct 模式 flush 指针 + 80 MHz

- **现象**：整屏花屏 → 修指针偏移后变成"刷新的部位有时候花屏"。
- **根因 1**：esp_lvgl_port 2.9 在 SPI 屏的 direct 模式下，把整块 framebuffer
  起始指针 + 局部区域直接交给 `esp_lcd_panel_draw_bitmap`，没有按区域原点偏移。
- **根因 2**：QSPI 跑 80 MHz，官方 BSP 只用 40 MHz；柔性排线裕量不足会随机损坏。
- **修改**：
  - 放弃 direct 模式，改为官方 BSP 的局部渲染 + 全屏缓冲方案；
  - QSPI 降到 **40 MHz**。
- **结果**：花屏消失（用户确认）。

### 4.2 白屏 / 启动崩溃 —— 内部 RAM 耗尽

- **现象 A**：启动即 `abort()`，backtrace 落在
  `fopen → __sfp → lock_init_generic`（newlib FILE 锁分配失败）。
- **现象 B**：`spi_master: setup_dma_priv_buffer: Failed to allocate priv TX buffer`
  → `flush failed (ESP_ERR_NO_MEM)` → 白屏。
- **根因**：内部 RAM 被吃光：
  - D-cache 64 KB + I-cache 16 KB（后来 32 KB）、第二渲染线程栈 32 KB、
    SRAM band 52 KB、WiFi 默认缓冲等叠加；
  - `SPIRAM_MALLOC_ALWAYSINTERNAL=1024` 让所有 ≤1 KB 的分配都占内部 RAM。
  - PSRAM 绘制缓冲的 SPI 传输需要**内部 DMA 弹跳缓冲**（每次传输分配/释放），
    内部 RAM 不足 8 KB 时直接失败。
- **修改**（对齐 pai-touch/nertc 的内存策略）：
  - `SPIRAM_MALLOC_ALWAYSINTERNAL=0` + `SPIRAM_MALLOC_RESERVE_INTERNAL=262144`：
    普通分配（含 LVGL 对象、newlib 锁）可落 PSRAM，内部 RAM 只留给
    DMA 缓冲、任务栈、WiFi 静态池；
  - WiFi 缓冲按 nertc 裁剪：静态 RX 6 / 静态 TX 8 / 动态 RX 6 / recvmbox 24；
  - SPI 分块从 21 KB 降到 **8 KB**，弹跳缓冲一定能分配成功。
- **结果**：白屏与崩溃消失。

### 4.3 看门狗 —— LVGL 忙等 flush

- **现象**：`task_wdt: taskLVGL (CPU 1)` 每 5 s 触发，backtrace 为
  `refr_invalid_areas → draw_buf_flush → wait_for_flushing`。
- **根因**：LVGL 9.5 的 `wait_for_flushing` 在没有 `flush_wait_cb` 时是
  `while(disp->flushing);` 忙等；`flushing` 只在传输完成回调里清零。
  上面 4.2 的提交失败没有回调 → 永远等 → 看门狗。
- **修改**：板级接管 flush 回调，提交失败时打日志并主动
  `lv_display_flush_ready()`；后续又把提交移入刷屏任务（见 4.5）。

### 4.4 渲染并行与缓存调优

| 改动 | 说明 |
| --- | --- |
| `LV_DRAW_SW_DRAW_UNIT_CNT=2` | 整屏切 2 个 tile，两个渲染线程跑两核 |
| `LV_DRAW_THREAD_PRIO=4` | 默认 3 与 LVGL 任务同级；提到最高档，渲染线程可抢占 |
| I-cache 32 KB / D-cache 32 KB / 64B 线 | 对齐 pai-touch；D-cache 试过 64 KB，省了 32 KB 给堆更划算 |
| `LV_CACHE_DEF_SIZE=1 MB` | 原来是 **0（关闭图像缓存）**，图标每帧重新解码；开启后稳定 |
| `LV_IMAGE_HEADER_CACHE_DEF_CNT=32` | 图像头也缓存 |

### 4.5 刷屏任务（bounce copy 移出 LVGL 任务）

- **动机**：全屏帧提交时，SPI 驱动要对 8 KB × 42 段做 PSRAM→内部弹跳拷贝，
  约 12~18 ms 全部发生在 LVGL 任务里。
- **修改**：板级 flush 回调只入队（`显示/区域/缓冲`），`watcher_flush` 任务负责
  `esp_lcd_panel_draw_bitmap`；成功仍由 panel IO 完成回调释放 LVGL。
- **结果**：flush 从 12~18 ms 降到 **0~1 ms**（LVGL 侧），帧时间整体缩短。

### 4.6 参考 UI 侧：去掉半透明遮罩（最大单项收益）

- **现象**：通知栏拖动 render 50~68 ms，是所有场景里最慢的。
- **根因**：`notification_shade` 是一块**全屏半透明 scrim**，下拉时盖在
  （被整块重绘的）启动器上；scrim 透明度 16 级跳变还会触发整屏失效重排。
- **修改**（`deps/pxa-system/ui/reference/lvgl/src/reference_lvgl.c`）：
  - shade 背景改为**全透明**，删除透明度动画及量化逻辑；
  - 保留点击关闭的点击区。
- **结果**：render **50~68 ms → 26~40 ms**，FPS **7~8 → 23~30**。
- 曾尝试"快照替代live内容"方案（`lv_snapshot_take` + 隐藏背景对象），
  已按产品要求**回退**，不再使用。

### 4.7 面板 `clip_corner` 关闭

- **根因**：全屏通知面板 `radius = base_radius×3` 且 `clip_corner=true`，
  面板内每个子对象绘制都要走一遍圆角遮罩。
- **修改**：`lv_obj_set_style_clip_corner(..., false)`，背景圆角保留，
  子对象不再裁剪（面板是全屏的，圆角在屏幕角落，无子对象压到那里）。
- **结果**：render 再降数 ms，通知栏稳定在 **23~36 FPS**。

---

## 5. 历史 UI 实测数据（同一帧周期内取样）

| 场景 | render | flush | FPS |
| --- | --- | --- | --- |
| 通知栏下拉（优化前，含 scrim） | 50~68 ms | 12~18 ms | 7~8 |
| 通知栏下拉（去 scrim + clip_corner） | **23~40 ms** | **0~1 ms** | **23~36** |
| 设置页滚动 | 16~24 ms | 5~7 ms | 32~37 |
| 待机（无重绘） | 0 ms | 0 ms | 56~60（refr 空转） |

内存（启动后）：

- 内部 RAM：充足（预留水位 256 KB，只服务 DMA/栈/WiFi 静态池）；
- PSRAM：8 MB 中约 1.2 MB 已被运行时占用，主要是
  **LVGL 双帧缓冲 663 KB**、图像缓存（上限 1 MB，按需）、LVGL 对象、WiFi 动态缓冲。

---

## 6. 触摸链路（与显示优化同期完成，简述）

- **驱动 vendor 化**：`src/watcher_spd2010_touch.c` 从官方 2.0.1 复制并打补丁：
  暴露 `pressed / pt_exist / read_len / gesture`，跨调用保持按下状态，
  修正抬手记录被 `pt_exist` 条件丢弃的问题，并对 `read_len` 做缓冲越界保护。
- **事件驱动读取**：扩展器 INT（GPIO2）任意边沿中断 → 信号量 →
  独立触摸任务（优先级 10）立刻读 I2C；50 ms 兜底轮询；
  LVGL indev 回调只发布缓存状态（6 ms 定时器）。
- **释放判定**：控制器明确 `w0=0` 抬手记录 → 立即释放；
  按住期间连续 **300 ms 无任何上报** → 判定松开（手指存活时约 16 ms 心跳）。
- **多指**：5 个 pointer indev，按控制器 `track_id` 绑定 slot
  （120 ms 宽限），guest 侧 `pointer_id` 稳定为 0~4。

---

## 7. 关键经验（容易踩的坑）

1. **esp_lvgl_port 的 direct 模式在 SPI 屏上不会偏移 flush 指针**，
   局部刷新会花屏；要么自己写 flush，要么用局部渲染 + 全屏缓冲。
2. **LVGL 的 `wait_for_flushing` 是忙等**（未设置 `flush_wait_cb` 时）：
   正常和失败路径都必须发布完成；当前使用让出调度的等待回调，由 LVGL owner 自身清除 flushing。
3. **PSRAM 绘制缓冲的 SPI 传输要走内部 DMA 弹跳缓冲**，
   小块（8 KB）比大块（21 KB）安全得多；内部 RAM 不足时的报错是
   `Failed to allocate priv TX buffer`。
4. **`SPIRAM_MALLOC_ALWAYSINTERNAL=0` + 高水位线**是内部 RAM 紧张时的正解：
   小分配（LVGL 对象、newlib 锁）落 PSRAM，内部 RAM 留给 DMA/栈。
5. **`LV_CACHE_DEF_SIZE=0` 等于没有图像缓存**，图标会在每次重绘时重新解码；
   开启即可（1 MB 足够）。
6. **半透明全屏遮罩是性能杀手**：一帧一次全屏 alpha 混合，
   透明度变化还会整屏失效；能不做就不做。
7. **全屏对象的 `clip_corner` 会为每个子对象加圆角遮罩**，
   软件渲染下很贵；背景圆角（`radius`）不受影响。
8. **SRAM band 方案在 LVGL 9.5 不一定更快**：大面积失效时每帧被切多块，
   每块都有 tiling/线程同步固定开销；整屏缓冲 + 2 draw unit 更划算
   （本板实测 band 版 render 更差）。
9. **Watcher 没有 TE 引脚**，无法像 pai-touch 那样锁帧；撕裂只能缓解不能消除。

---

## 8. 遗留项与可选下一步

- **撕裂**：无 TE，写入与面板扫描异步，无法根除；当前撕裂程度与写入量成正比。
- **面板下方仍被重绘**：LVGL 无遮挡剔除，不透明面板下方的启动器在失效区域内
  仍会被重画；要彻底消除需要隐藏背景/快照（已明确不做）。
- **可选**：通知面板高度从整屏缩小（减少失效面积）；
  或对"面板滑动导致的整块失效"做更细的失效控制（需改 UI 结构）。
- **内存**：guest 应用启动后 WAMR 线性内存/AOT 代码还会占用 PSRAM，
  如需更多余量可下调 `LV_CACHE_DEF_SIZE` 或限制单实例内存。
