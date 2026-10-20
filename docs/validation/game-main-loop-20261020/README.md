# Core 2.1 游戏主循环：接口、体积与离线关卡验证

本轮已实现 Core 2.1 的事件端口、ESP Host 原生维护，以及五个游戏的可选主循环。
**没有新增真机帧率成绩，没有刷入新固件，没有完成完整原音频验收。** 烧录前同一
GameKit 出现另一项任务的 `pxa-nes 0.1.22 active=1`；保护检查在启动 esptool 前退出，
设备测试优先级已向用户询问，运行现场保留。

## 代码与设备边界

- SDK：`c14ce9d`，C 入口版本修正 `5a6e179`，已合入 `deps/pxa-system`。
- Host：隔离源提交 `9ef87b7`，当前工作分支对应 `fad27a0`。集成在共享固件构建锁内完成。
- Apps：`effafea`；启动时序修正 `71dc796`，已合入 `local/pxa-apps`。
- 探针清理与 Python 优化模式下的保护修正在当前分支单独提交，见工具文件历史。
- GameKit 序列号 `1c:29:04:d0:af:5e`，仅使用过 `/dev/ttyACM0` 的只读查询。
  `device-before.json` 是本轮开始时的快照；其后另一项 NES 测试改变了安装/运行状态，
  不能把该快照当成本轮结束状态。没有据此恢复、归档或删除任何设备包。
- `firmware-v10.json` 是构建完成的候选镜像，SHA-256
  `ac20808980d70e457133ede6b9d1303c73aecd9480ae53398ef33905972a3502`，
  3945760 字节；其分区表哈希与 v9 一致。镜像保存在 `local/`，未烧录。

## ABI 与调度

进口仍只有 `pxa.core.v2.pxa_call`、`pxa.core.v2.pxa_io`，WASI 声明保持原样。
签名包声明 `execution_model: main-loop`，要求至少 Core 2.1，且只能有一个 `main`
UI Component。执行能力来自实际配置的引擎，不能用提高 Core/服务版本代替。

Core opcode 5 打开实例私有的类型 15 句柄，I/O `0x201` 等待标准事件。缓冲由调用者
提供（20..4096 字节），前 8 字节编码微秒超时；零等待返回 WOULD_BLOCK，正等待到期
返回 TIMED_OUT，上限 60 秒。短缓冲保留队首与目标字节，取消直接终止 VM 调用。
运行中没有额外 Guest 线程、Host 队列或帧缓冲；复用已有 Runtime 任务栈。

ESP 在等待点处理有界输入、Clock、文件/资源、音频及权限完成，不进入其他 Guest。
生命周期或包变更命令保持在队首，旧 VM 调用返回后先销毁实例，再取走命令。
外层循环同一轮维护也不能提前消费该命令。看门狗排除原生等待，保留超期后的显式
处理与终止；零时间检查点也周期性让出 CPU。

游戏 `port_rpc` 等待匹配 token 时仍分发输入/生命周期。后台暂停；停止后不能恢复
已经丢失的调用栈。游戏只从前台 Clock 事件启动，资源或权限回复不再提前触发引擎。
`startup-negative.log` 证明新回归能检出先前候选的过早启动；最终夹具通过该检查。

## 验证结果与限制

SDK 重建后 **45/45** 测试通过，包括解释器与 x86 AOT 的超时、队首保留、跨实例/过期
句柄、memory.grow、取消、外部终止、超期延长、重入拒绝和零泄漏回收。
规范的 52 个 Core 向量、5 个包载荷及可重复生成检查通过。隔离与集成后的 ESP Host
完整测试均在 ASan/UBSan 下通过。设备不匹配、初始探针已运行和非法替换 ID 的三项
保护测试在普通 Python 与 `PYTHONOPTIMIZE=1` 下均通过，失败保护不发送清理操作。

原回调夹具与新主循环夹具都验证真实 C++ 字符串、虚拟文件、文件内容及后台暂停。
每次文件 I/O 加入 40ms 延迟，两条路径各检查 63 个合作边界。最终原生夹具额外要求
从 Clock 启动，通过 `FIXTURE PASS` 和 `BACKGROUND RESUME PASS`。这些是桌面协议测试。
原生夹具的 WebAssembly 内存比回调少 262144 字节，不能据此宣称设备固定线性内存
预留减少；游戏最大线性内存配置未被降低。

五个最终 Wasm 用相同原始资源在 Node 22 协议宿主检查。`final-node-*.png/json/log`
及 `final-inputs.json` 绑定输入字节码和 Apps 提交；每个最终运行均启用 Clock 启动
检查。Quake/Quake II 的 HP=100、实际关卡标志和变化坐标，Duke 的 mode=4 与方向
输入记录，以及两款 RTS 的实际地图截图用于排除菜单/加载画面。桌面宿主不播放
设备音频，也没有 LCD 完成帧计数，不能作为真机 FPS 或完整音频的证据。

最终签名 ESP32-S31 包的 AOT/容器哈希、体积和来源见 `final-artifacts.json`。这些是
真正使用事件端口 SDK 的包，不是此前残留协程进口的离线删除实验。静态 TEXT 加
线性内存只构成内存下界，仍缺目标设备的装载、堆/栈峰值及稳定运行验证。

## 重放

从本项目根目录设置 `PXSYS_ROOT`、`PXA_GAME_APPS_ROOT`、`WASI_SDK_DIR`、`WAMRC`
和开发签名密钥路径。SDK 与 Apps 使用上述对应提交，Binaryen 使用现有 130 版本。
对 `quake-main-loop`、`quake2-main-loop`、`duke-main-loop`、`cnc-main-loop`、
`ra-main-loop` 分别执行：

```sh
bash tools/app.sh build quake2-main-loop --board doit-gamekit \
  --source-root tools/game-perf/profiles --target esp32s31 --aot-only \
  --output local/game-main-loop-final
```

桌面验证使用保存的对应 Wasm、独立的 state 目录、原始资源目录和
`PXA_TEST_MAIN_LOOP=1 PXA_TEST_REQUIRE_TICK_START=1`。按 `final-node-replay.json` 的
持续时间与按键序列调用 `common/recrof/tests/host.mjs`；脚本输出明确标记为非设备 FPS。

设备空闲并确定测试顺序后，先验证候选固件与已签名 `main-loop-probe` 的启动、输入、
后台/恢复、连续停止及替换，记录堆与 Runtime 栈余量。再测试游戏的实际关卡、相同
默认画质音量的完整 60 秒窗口和前后截图。系统 BACK 在此执行模式中退出游戏，
游戏内菜单使用应用的触摸/手柄事件。

## 未完成工作

真机主循环探针、五个最终包的设备装载/输入/内存/帧率、至少 15fps 的完整最低验收
仍未完成。16kHz 单声道链路也尚未重构为源格式与设备格式分别协商；不能把当前
音频开启或桌面静默输出称为完整原音频。公开游戏默认仍为回调模型。
上游 vendor 没有新增修改；此次变更集中在公共运行时与构建入口，继续沿用既有
`common/recrof/tools/upstream.py` 的固定版本、差分补丁与同步流程。
