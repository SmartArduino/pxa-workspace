# PXA 架构、ABI 与 GameRender 重构记录

此文区分已经实现的改动与后续需要收敛的工作。`pxa.core.v1` 预览现已接通 Log、Device、Window、Permission、Storage、FS、IPC、Net、Audio、Sensor、Work、Surface、Clock、UI、GameRender、Store Installer 和 WASI 能力声明。签名包、20 字节 envelope、64 位请求 token、原生 64 位 Guest Handle、WAMR 导入和 Guest SDK 已端到端运行。包括两个烟测包在内的 17 个内置应用均已使用 v1。

## 架构判断

PXA 的包签名、Component 生命周期、Service 控制消息和 Handle 数据通道分层是合理的。真正影响正确性与维护成本的是：接受异步请求与预留完成事件曾分离，多个 Service 自行管理完成路径；Guest 可见 Request ID 与 Handle 都只有 32 位；WAMR 导入、Host 解码、Guest SDK 与 JSON 规格由多份手写定义维护；UI 构建器曾对每条小命令单独跨 Wasm 边界；GameRender 的资源上限和实际分配策略曾缺少对应的包级检查。

已经完成的重构把“接受后恰好一次完成”所需的事件容量提前预留，覆盖同步 Service、IPC 和 Net；取消在不可逆动作前生效，提交后继续交付完成事件。Host 内部 mailbox lease token 已改为 64 位，资源 Handle 的空闲槽使用 FIFO 轮转以延缓 16 位 generation 耗尽。UI 小命令合并写入一次 `TX_WRITE`，大值按边界分片；Host 的 UI 内存可按配额增长，ESP 配额由 Kconfig 控制。GameRender 现在可按场景选择 `DEPTH16`、无 scratch 或 2-bit coverage，并在创建时限制 DrawList 容量。这些是 v0 运行时改进，不改变 v0 wire 格式。

## Core v1 ABI 与剩余迁移

v1 控制 envelope 固定 20 字节，包含 service/opcode、64 位请求 token、payload 长度及 flags。Host 通过签名包的 Core major 约束 WAMR 导入；单向命令使用零 token，需完成事件的请求使用非零 64 位 token 并原样返回。当前每个 Component 最多暂存 16 个 v1 请求映射，满额时在 Host 派发前拒绝。Host 资源表已支持 32 位 generation、64 位 Handle 与统一的所有权、类型、权限检查；v0 仍可走 32 位接口，超过 16 位 generation 的槽会被 v0 分配跳过。签名包验证后的 Core major 记录在 Component 上；已迁移的资源 Service 使用原生 64 位 Handle 和对应的结果布局。

具体迁移顺序：

1. v0/v1 envelope 与 v0 record 已从规格生成相同的 Host/Guest codec，38 个 v0 消息、13 组 record golden vectors 与 v1 样例在两侧逐字节通过。Device runtime-info 的 Host 编码、v0/v1 Guest 解码也已改用共源生成的 Service codec，并核对结果 golden、字段顺序、UTF-8 与长度边界。成功结果按 schema 最多 180 B，Host 的栈缓冲与每请求完成事件预留已从 256 B 降为 180 B。Window 快照的 Host/Guest codec 也已共源生成，固定结果为 98 B；其他 Service payload 仍待迁移。
2. 资源表、GameRender 创建与 typed I/O 的 64 位接口及 generation 溢出、跨 Component、跨类型和权限撤销测试已落地；继续把其他 Service 结果和创建路径升为原生 64 位 Handle。
3. v1 Core 现可用 64 位目标 token 取消请求；WAMR 延迟完成 fixture 验证提交前恰好一次 `CANCELLED`，签名包验证对已排队完成的请求取消不会覆盖原结果。当前签名 Guest 还验证了 16 个并发 token、满表拒绝、槽位复用和 17 次完成。继续将请求表扩展到其他异步服务，并验证跨服务取消与压力路径。
4. 已迁移 SDK、所有内置应用及模拟器 fixture；全部应用包声明 Core v1，Store 与 Lab 还通过 ESP32-S3 和 ESP32-S31 的 AoT 交叉编译。v0 导入与部分手写 Service codec 继续作为外部旧包兼容层保留，下一次收敛应按实际发布包迁移情况决定淘汰时间，并优先把剩余 Service codec 纳入规格生成。不能靠 envelope 单测宣布设备端稳定。

## Guest SDK 目标

SDK 应提供无堆分配的定长 builder 和 typed decoder。builder 只检查并生成消息，不暗中提交；`send` 显式返回 Host 即时状态。应用自带固定容量 pending 表，也可直接按 token 分发。解码器同时核对 service、opcode、token、status 和结果长度，事件视图仅在回调期间有效。UI 的事务 helper 应保证失败后不可继续 commit；GameRender helper 应把尺寸、scratch、最大 DrawList 字节数放进明确的 options 结构。SDK 现覆盖上述已迁移的 Service；高层 UI 事务 helper 直接使用当前传输（没有传输开关宏），GameRender 的 DrawList 编码可通过 `pxa_raster.h` 复用，并保留调用方缓冲区所有权。应用的跨服务状态管理与迁移仍待继续收敛。

v1 SDK 还增加了调用方提供存储的固定容量 pending helper：显式提交前占用 token，即时拒绝时撤销，回调按 token、service 和 opcode 一次性取出用户上下文；不引入堆分配。

## GameRender 内存与性能

ESP GameRender 使用 3 个 RGB565 帧缓冲、1 个 scratch 缓冲及 3 个按需增长的 DrawList mailbox，主要在 PSRAM。按当前 296×240 显示与 16-bit depth 计算，不含纹理、调色板、对齐和分配器开销：

| 渲染目标 | 帧缓冲 | depth | mailbox 初值 | 合计下限 |
| --- | ---: | ---: | ---: | ---: |
| 296×240 原生 | 426,240 B | 142,080 B | 12,288 B | 580,608 B |
| 148×120，2 倍呈现 | 106,560 B | 35,520 B | 12,288 B | 154,368 B |

因此 2 倍呈现少用 426,240 B 固定工作集，需栅格化的目标像素最多降到四分之一。无 scratch 可再省 35,520 B；2-bit coverage 的 scratch 为 4,560 B，较 depth16 少 30,960 B。选择 scratch 必须符合 DrawList 的深度语义，不能为省内存静默丢弃 depth test。显式 `max_draw_bytes` 可防止三个 mailbox 从默认 4 KiB 持续增长到各 48 KiB。

桌面产品模拟器运行签名基准包的 11 组场景。2 倍呈现得到约 53–62 FPS、Host 光栅时间 0–1 ms；原生分辨率最重的 `GM-3D` 一次运行是 51 FPS、4 ms，另一运行是 62 FPS、2 ms。模拟器使用桌面 CPU、SDL dummy 显示及不同的缓冲布局，这些数字仅说明基准能端到端运行，不能推断 ESP32 实机性能。桌面呈现路径已避免满屏画面每帧清空 142,080 B，并使 1:1 拷贝走 `memcpy`；串行光栅也改为共用一份 depth scratch，原生分辨率少分配 142,080 B。桌面与 ESP 后端现允许无调色板的纯色清屏 DrawList，省去没有纹理时多余的调色板分配与上传；ESP Host 桩验证了实际清屏像素。主机调度抖动大，不能从单次基准推断其 FPS 收益。

`tools/dev.sh sim --board pai-touch voxel-craft` 使用桌面 CPU/WAMR/LVGL，不模拟 ESP32 主频。该开发入口原先没有设置 CMake build type，Host 栅格以未优化 C 代码运行；现在桌面模拟器默认 Release，可用 `PXA_SIMULATOR_BUILD_TYPE=Debug` 切换。进一步用 `PXA_SIMULATOR_PERF=1` 分解主循环耗时发现，`pai-touch` 圆角遮罩逐扫描线提交 LVGL 矩形曾在游戏时占约 44–46 ms/轮，使正式 Voxel Craft 包降至约 16–19 FPS。改为启动时预生成一张 ARGB8888 透明遮罩后，同一板型与包在桌面窗口复测 LVGL 为约 0.3–0.5 ms/轮，游戏约 30 FPS；截图确认圆角底色与触摸菜单正确。这是桌面模拟器性能修复，不代表 ESP 设备帧率。

## 验收边界

最新验证包括从 system 根目录完整构建及 64/64 项 CTest、独立桌面构建及 10/10 项 CTest。system 根目录构建曾因 LVGL adapter 在依赖初始化前未建立而链接失败；现已共用 LVGL 初始化逻辑，两种构建入口均通过。

v1 Permission `CHECK/ACQUIRE` 和 Device `GET_MAC` 已端到端运行：签名包从声明的权限取得原生 64 位 Handle，并用它读取模拟器的 MAC；作用域不符、篡改代际与关闭后的旧 Handle 均被拒绝。Host 核心测试验证 `resolve64` 和旧 32 位入口隔离。

v1 预览开放 Window `GET_SNAPSHOT`、`CONFIGURE` 与 `SHOW_TOAST`：Guest 用 64 位 token 请求快照，单向命令使用零 token。签名包验证了真实快照结果、完整的配置与提示调用，以及 Device 请求占满 16 个槽时的跨服务拒绝。Window 快照的 Host 编码与 Guest 解码现已从同一份 schema 生成；固定偏移的 98 B codec 在两侧与 golden 逐字节一致，并检查字段顺序、长度、截断及非法值。

Storage v1 现支持 `GET/SET/REMOVE/LIST`。签名包写入并读回桌面 Host 允许的 2048 B 最大值，再验证列表、删除与缺失键。WAMR 派发改为只复制当前请求的 payload，并直接调用 Host 已解码消息入口，去掉了为所有 v1 请求预留固定大栈缓冲及重编码 v0 envelope 的步骤。ESP 固件目前每个值配置上限 960 B；尚未用实机测量栈峰值。

私有 FS v1 已迁移 `open` 的原生 64 位文件与目录 Handle，`seek` 和目录读取改用 64 位输入；typed I/O 与 Core close 直接查询 Host 资源表。桌面产品模拟器现按签名发布者和 App ID 隔离 POSIX FS 根与 Storage 数据目录。签名包验证目录与文件创建、写读、seek、stat、目录遍历、rename、删除，以及伪造代际与关闭后的旧 Handle 拒绝。v0/v1 Guest FS 共同使用一份路径 UTF-8 校验实现。
ESP Host 仅在包内组件声明 FS 时分配并注册其私有文件系统，避免不使用 FS 的应用常驻文件句柄表和 POSIX 后端工作区；桌面签名包测试还核对发布者与 App ID 组成的数据目录名。

IPC v1 已接通两个 Component 的异步调用与回复。普通完成事件保留 Guest 的完整 64 位请求 token；IPC `request/result` 通知则将 Broker 的 32 位 call ID 零扩展到 v1 envelope 的 token 字段。Guest SDK 用不同的 typed decoder 区分两者。WAMR 双组件 fixture 和签名产品模拟器包均验证按需激活提供方、请求转发、回复以及结果送达。call ID 仍是 32 位，属于继续迁移时需要消除的旧协议限制。

现有 `wasi-lab` 已迁移到高层 UI v1 传输与 Window v1。桌面模拟器补报与 ESP 一致的 WASI clocks/random 能力；签名 WASI 包运行后，截图显示 6/6 检查通过及 UI 正常呈现。`game-render-bench` 已迁移到 GameRender、Clock、Log v1；共享 DrawList 编码器按 Core 版本选用 64 位 Handle 的 I/O。应用的 DrawList 容量从协议上限 48 KiB 收紧到 8 KiB，三个 Host mailbox 的理论预留上限因此从 144 KiB 降到 24 KiB；此处仅计算 DrawList，不包含帧缓冲、depth、纹理和分配器开销。2× 缩放和原生分辨率的完整 11 组均在桌面模拟器通过。

Canvas SDK 已新增 `pxa_canvas.h`：绘图记录编码保持一份，事务和 Canvas 写入走 v1 envelope，Stream 就绪事件及 I/O 使用原生 64 位 Handle，长帧在调用方缓冲中直接分片。单测覆盖高 32 位非零的 Handle、Stream I/O、无 Stream 分片和指针事件。内置 Canvas 游戏已随各自的时钟、音频、存储及事件状态机迁移，并声明 Core v1。

`maze-spike` 已作为第一个 Surface 游戏迁移到 v1。Mapped RGB565 三缓冲继续使用调用方固定内存，Surface 创建、配置、查询、释放及 typed I/O 均走原生 64 位 Handle；时钟 `now` 只保留一个未完成 token，防止重复提交污染渲染耗时。签名包仅导入 `pxa.core.v1`，桌面模拟器截图确认画面和约 30 FPS 的事件循环。原生 ESP 的 DMA、PSRAM 和屏幕呈现时延仍待设备测试。

`plane-shooter-raster` 也已使用 v1 GameRender、UI、Window 与 Clock。它保留 8 KiB 的 DrawList 上限，绘图编码器直接使用 64 位上下文 I/O；签名包只导入 `pxa.core.v1`，模拟器验证了战斗画面和持续帧循环。

`tomb-explorer` 使用同一套 v1 Canvas 输入和 Raster 绘图入口，指针与手柄事件现在由高层 SDK 做字段校验。间接包含 v0 UI/Raster 头文件会触发明确的编译错误，防止签名 v1 包意外混入 v0 导入。其签名包在模拟器显示了三维场景，并且导入表仅包含 `pxa.core.v1`。

`jump-jump-3d` 已迁移到 v1 GameRender、UI、Clock、Window、Storage、Permission 和 Audio。音频与渲染上下文均使用原生 64 位 Handle；SDK 时钟 helper 限制长延迟后的补帧步数。模拟器确认音频会话就绪、交互画面与最高分持久化。静止窗口 DrawList 峰值 7,292 B，12 次连续触摸窗口峰值 10,300 B；应用把 DrawList 上限由 48 KiB 收紧到 16 KiB，因此 Guest 固定缓冲减少 32 KiB、Host 三个 DrawList 槽位的容量上限减少 96 KiB。这些是配置容量差额，不是实测 ESP 内存或性能数据。

`garden-guard` 已迁移到 v1 Canvas、UI 输入、Clock、Window、Storage、Permission 和 Audio。共享 `pxa_game_sfx_v1.h` 使已有合成器通过 v1 会话与原生 64 位 Handle 写入 PCM，保留 v0 调用方。模拟器验证首页与触摸进入选卡界面；v1 音效握手测试覆盖许可、会话、音频图及帧提交，原有 v0 音效测试继续运行。

`plane-shooter` 的菜单与战斗模块已共用 v1 Canvas、Clock、UI 输入和 64 位音频会话，进度存储使用 v1 Storage。产品模拟器验证进入战斗、连续帧与触摸；菜单截图与同版本 v0 包逐字节相同。战斗画面的部分文字布局异常在 v0 基线也存在，是独立的既有 UI 问题。

`maze-evil` 的 RGB565 映射三缓冲、Surface 状态查询与释放事件、Clock 计时、Canvas 输入及自定义音频均已迁移到 v1；音调优先、PCM 回退继续由同一个音频状态机管理。产品模拟器确认开场、触摸进入游戏和音频会话就绪。v0/v1 Surface 共用一份不溢出的显示缩放计算，避免仅为屏幕坐标映射而包含旧 ABI 头文件。此处仍保留非映射 Surface 回退；是否去掉该回退以减少静态缓冲，需要硬件和产品能力矩阵确认。

`pixel-dungeon` 已迁移 GameRender、Canvas/UI、Clock、Window、Storage、Permission 与 Audio，自动尺寸创建失败仍会尝试默认比例和显式尺寸。连续存档写入使用递增 64 位 token，避免尚未完成的写入重复占用同一请求号。签名包在产品模拟器中呈现标题和存档选择页，原版音乐资产加载、音频会话建立和 GameRender 296×240 首帧均成功；完整 Guest SDK/App 测试通过。尚无 ESP 实机帧耗时或实际内存测量。

所有迁移后的 App 已去掉 `pxa_app_stop` 中无效的控制与 I/O import：生命周期规范规定 stop 只是通知，Host 自动回收 Handle。`pixel-dungeon` 把用户按返回键时的存档提前到事件回调，继续保留游戏内定期检查点；突然断电仍只保证最近一次成功的检查点。

`arcade` 的六个小游戏共用 v1 Canvas、Clock、Window、Permission/Audio 音效通道。它们不会同时运行，因此绘图缓冲、UI 命令缓冲及消息缓冲改为同一组静态数组；按源代码声明的容量计算，常驻 Guest 缓冲减少约 101,878 B。签名包在产品模拟器中显示菜单并进入打砖块，旧的独立小游戏测试入口仍保留。上述字节数是静态容量差额，不是设备实测峰值。

`weather` 已迁移 v1 UI、Window、Storage、Permission、Net 和 Clock；权限、HTTP body 改用原生 64 位 Handle，读取结束后通过 v1 Close 释放。产品模拟器中的签名包完成定位、网络天气获取并显示 San Jose 天气，确认实际 HTTP 流读取路径已运行。该结果仅覆盖模拟器当前网络环境，离线与各备选天气源仍靠现有回退逻辑及解析器测试验证。

`voxel-craft` 已把签名包的 GameRender、UI、Window、Clock、Storage、Permission/Audio、Log 路径切至 v1。原生 64 位上下文直接用于纹理上传、DrawList 提交和 telemetry 查询；Storage 的 2 KiB 分块使用调用方独有的 2304 B 封包，不再维护第二份 2304 B payload。发布构建仅采用 GameRender；旧映射 Surface 回退在编译条件之外，因此不分配三块最大 320×240 RGB565 帧缓冲（声明容量共 460,800 B）。签名包在产品模拟器呈现菜单和可操作的 3D 场景，日志中稳定场景样本的 DrawList 为约 37.2 KiB，Guest 光栅构建约 0.47 ms、Host 光栅约 1.8 ms、帧率约 30 fps；这些仅是桌面模拟器样本，不能外推 ESP 性能。

`store` 已切换 v1 UI、Window、Device、Permission、Net、Clock 与安装器 Service 20。下载进度保留 Host 内部 32 位请求号，但 WAMR 边界把它映射成 Guest 原始 64 位 token；签名包在桌面产品模拟器显示目录页面，模拟安装器提供空列表并对设备专有安装动作返回不支持。客户端改为直接复用封包缓冲，删去不再使用的 1024 B 静态 payload 数组和传入虚设缓冲区的参数。安装器控制路径在 ESP Host 只授权已安装、启用的内置 `pxa-store` 包；普通签名包即使声明 Service 20 也会被拒绝。v1 的安装、下载与列表请求在被接受时预留完成事件容量，避免稍后因事件队列满而丢失完成；用户提示或工作线程创建失败时也通过已预留的完成事件报告错误。Guest SDK 单测和 WAMR Host 桩分别验证 v1 封包、进度解析与高位非零 token 映射。真实下载、用户确认和安装持久化仍需 ESP 实机验证。

`lab` 从 18 个 v0 交互模块改为六项集中式 v1 健康检查，减少重复状态机和未使用的模块代码。它直接验证 WASI 时钟/随机数与动态分配、Device/Clock 的 64 位 token 完成、Storage 的写读删及私有 FS 的 64 位 Handle 写读删；每项结果显示在 UI 并写入 Host Log。签名包在产品模拟器中六项全部 PASS。旧 Lab 的音频、网络、Surface、后台任务和 IPC 交互演示不再作为这个包的功能；对应 v1 路径由专用应用、SDK 测试与签名烟测覆盖。原 Lab 的 AOT/Wasm 微基准也已移除，GameRender 性能由专门的 `game-render-bench` 测量。

打包器也开始直接解析 Wasm 导入段，并在 AoT 编译和签名前核对声明的 Core major 与 WASI 能力。声明 v1 却导入 `pxa.core.v0`、或使用 `clock_time_get` 却未同时声明墙钟与单调时钟的包，现在都会在构建时失败，而不是直到激活时才暴露问题。最终 17 个签名模拟器包的 Wasm 导入均再次通过能力检查。

此外，ESP 适配层主机桩测试、完整 Guest SDK/App 测试、包生成器测试以及合并规格检查均已通过。签名 `abi-v1-smoke` 包在产品模拟器内执行了 16 个并发 64 位 token、满表拒绝、槽位复用与 17 次完成；随后用 GameRender 创建、纯色 DrawList 提交、telemetry I/O、创建失败后的完成事件、关闭和重建验证 64 位 Handle、篡改代际和旧 Handle 拒绝，并覆盖重复 token、非法 flags/token/I/O 拒绝路径。声明 v1 却导入 v0，以及 v1 包请求未迁移的服务，也都被拒绝。该压力测试还发现并修复了模拟器在窗口指标事件被满队列拒绝后不再派发完成事件的死锁。v1 Guest SDK 头文件现可用裸 Wasm 工具链编译，无需标准 C 库头文件。签名 `game-render-bench` 包在产品模拟器内完成全部 11 组，覆盖两种分辨率。复现原生分辨率包可使用：

```sh
PXA_APP_SOURCE_ROOT=local/pxa-apps \
PXA_APP_DEFINES=PXA_GAME_RENDER_SCALE_SHIFT=0 \
PXA_PACKAGE_OUTPUT_ROOT=/tmp/pxa-bench-native \
bash deps/pxa-system/tools/package/package_app.sh \
  game-render-bench simulator /tmp/pxa-bench-native/pxa-game-render-bench
SDL_VIDEODRIVER=dummy timeout 65s \
  deps/pxa-system/build/simulator/desktop/pxsys_product_simulator \
  --package /tmp/pxa-bench-native/pxa-game-render-bench \
  --publisher-key local/pxa-apps/.dev-signing/publisher-public.der
```

无 ESP 设备时可验证 Host/Guest codec、配额与失败路径、签名包在产品模拟器内执行、视觉快照以及桌面进程 RSS。ESP32-S3 `pai-touch` 固件也已完整编译与链接，17 个内置应用均重新生成模拟器签名包，Lab 与 Store 的 ESP32-S3/ESP32-S31 AoT 包成功生成。实机验收仍需采集 p50/p95/p99 呈现延迟、Host 栅格耗时、内部 RAM/PSRAM 峰值、纹理上传与 DMA cache 同步耗时，以及队列压力下的完成事件丢失数。没有这些测量，就不应宣称硬件帧率或内存峰值已达标。

## ESP 应用生命周期收敛

ESP Host 现在把任务前后台状态与板级屏幕解锁状态合成 UI 组件的有效前台状态。状态在 Host 内部覆盖更新，而非为每次状态变化分配一条命令；Guest 的生命周期通知仍使用 System 事件，但失败时保留目标状态并重试。后台和锁屏时，Host 暂停 UI 组件的 Clock 槽位并丢弃旧代际 tick，暂停 PCM 与资产音频；Work 组件的时钟槽位不受 UI 暂停影响。恢复时重新安排下一次 tick，不补跑后台累计帧。

新启动的 App 在系统首次置前台之前保持时钟和音频暂停，不会收到虚假的后台事件。Back 默认退出、外部 Stop、切换 Package 和关机/重启会先进入有界的后台保存阶段，继续派发 Guest 事件和已受理请求的完成事件；待 UI 组件无待处理请求与事件，或 1.2 秒超时后再停止。`voxel-craft`、`pixel-dungeon` 在后台通知中开始存档。关机/重启由板级独立任务等待 Host 完成停止，最长等待受限，不保留可能已删除的任务句柄。停止原因经桥接层传到 Guest 与系统任务管理器，最近任务中已失效的实例不能再假装前台恢复；运行时停止事件用 Host 实例 ID 精确匹配，避免同一 App 连续启动时误停新实例。

生命周期事件频率极低，不是主要性能瓶颈。帧 Clock 仍通过队列送入 Guest，但每个槽位只允许一个未消费 tick，过期周期合并，后台从源头停发。ESP 真机上仍需测量关机前 LittleFS 写入耗时、音频 sink 的暂停响应，以及极端队列压力下的停止超时行为。
