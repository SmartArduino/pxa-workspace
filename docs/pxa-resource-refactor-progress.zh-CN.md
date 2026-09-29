# PXA 资源重构执行记录

本文按阶段记录实际落地状态，较早章节中的“尚未”描述该阶段结束时的状态；最新状态见末尾。本文不替代 `pxa-resource-refactor-goal.zh-CN.md` 的完整目标与验收条件。整体目标尚未完成。

清理说明：文中 `tools/compare-*.py` 等对比命令是当时测量过程的历史记录；一次性对比及可行性审计脚本已经移除，这些命令不再是当前重跑入口。原始报告与测量结论仍按各阶段记录保留。

## 基线

- 开始时根仓库 HEAD：`e6bb9d9676199121a6d8b40ecdd3fefb093ded35`。
- 开始时 pxa-system HEAD：`e2de55b00e0e9a161ce27fc0c85db33b9ef78b81`。
- 两个仓库及本地应用库存在此前的修改。`build/resource-refactor/baseline/workspace.json` 记录 HEAD、状态、修改文件的大小与 SHA-256；该目录另存 tracked diff 和改造前的 ESP surface、desktop runner、raster 源码。
- 改造前运行 `bash firmware/components/pxa/tests/test_host.sh` 通过，日志为 `build/resource-refactor/baseline/host-tests.log`。
- 旧实现把纹理像素复制到后端，并在第一次提交帧后禁止更新。显示任务读取当前绑定，没有独立帧引用。桌面绘制前重复校验已经在提交时校验过的 DrawList。
- voxel-craft 已在修改应用前重新构建 simulator 与 ESP32-S3 包，详见下文。Guest 实际高水位、真实应用加载峰值、音频欠载等运行时基线仍需在应用迁移前补齐；静态声明的内存上下限不能代替运行时峰值。

## 已落地：资源所有权与帧快照

- `libpxa/include/pxa/raster_assets.h` / `src/services/surface/raster_assets.c` 提供 Host 不可变像素对象和引用集合。一个对象包含元数据与像素，使用一次分配；快照只增加引用，不分配或复制像素。
- ESP 与 desktop 的动态上传都已接入。上传替换未来帧的绑定，提交中的、排队中的、执行中的帧保留自己的旧纹理和调色板；失败提交不会破坏有效待绘制帧。
- 丢帧、执行完毕和上下文关闭释放对应引用。CPU 绘制完成就释放源资源引用，无需等显示帧退役。ESP 在临界区外分配和释放内存，上传准备期间也保护 surface 生命周期。
- desktop 保存提交时校验结果，绘制时直接执行，移除重复校验。
- 本步仍通过现有 Guest 上传入口创建像素对象。文件加载、句柄绑定、硬预算与资源缓存尚未接入；不能据此宣称已减少应用资源内存。

## 当前验证

- ESP 主机回归通过，包括共享场景测试：旧帧排队后替换调色板和纹理，旧帧像素保持红色，后续帧使用新颜色；丢弃旧待绘制帧；非法上传不破坏已有绑定；带排队帧退出。
- ESP 另有确定性执行交错：绘制已开始时上传新调色板，以及绘制已开始时关闭上下文。所有分配清零，分配与释放不能发生在模拟临界区内。
- desktop 的 `pxsys_raster_test` 在实际 LVGL/runner 后端运行同一场景并检查输出像素；`pxsys_audio_test` 同时通过。
- 独立 libpxa Debug 构建的 13 项测试全部通过。
- 新资源所有权测试及实际 ESP surface 主机测试通过 ASan/UBSan。
- `pxsys_product_simulator` 与 `pxsys_desktop_simulator` 构建通过。此次尚未进行两个 ESP 板的最终构建，也尚未迁移并运行实际应用。

日志位于 `build/resource-refactor/`：`host-snapshots.log`、`libpxa-tests.log`、`simulator-tests.log`、`asan-tests.log` 与相应构建日志。

## 固定工作负载性能记录

`simulator/desktop/tests/raster_benchmark.c` 使用实际 desktop 后端，Release 构建，240×240 RGB565 输出，20 张 256×256 INDEX8 驻留纹理，4 张可见纹理分别绘制到 120×120 区域，无 VSync。每轮预热 200 帧，记录 3000 帧。旧后端来自开始时保存的 `product_runner.c`，新旧使用相同光栅内核与同一测试程序。

固定 CPU 亲和性、交替运行新旧后端，各运行 5 轮。下表是各轮统计值的中位数，单位微秒：

| 指标 | 旧后端 | 帧快照后端 |
| --- | ---: | ---: |
| 提交 p50 / p95 | 1 / 2 | 2 / 2 |
| 光栅化及提交给 LVGL p50 / p95 | 153 / 168 | 153 / 166 |
| 总时间 p50 / p95 | 155 / 169 | 155 / 168 |

所有轮次最终像素校验值都是 `dca27e35`。一次测量对出现了明显主机调度/频率噪声，完整数据保留在 `bench-{baseline,current}-*.csv`、对应 `.log` 和 `bench-summary.json`；不能从微秒级差异声称获得稳定加速。目前未观察到重复出现的超过 5% 回退。这只是所有权改造的主机回归证据，不是资源重构最终基准，也不能推算 ESP FPS。

复现构建（在工作区根目录）：

```sh
cmake -S deps/pxa-system/simulator/desktop -B build/simulator/pai-touch \
  -DPXSYS_RASTER_BASELINE_SOURCE="$PWD/build/resource-refactor/baseline/product_runner.c"
cmake --build build/simulator/pai-touch \
  --target pxsys_raster_bench_current pxsys_raster_bench_baseline -j4
build/simulator/pai-touch/pxsys_raster_bench_baseline 3000 > baseline.csv
build/simulator/pai-touch/pxsys_raster_bench_current 3000 > current.csv
```

比较时选当前系统允许的同一 CPU，使用 `taskset -c <CPU>` 分别运行，交替顺序并重复多轮；以上 CSV 包含每帧原始样本。该基准尚未覆盖文件加载、换场景或慢存储。

## 已落地：资源编译、索引与分块加载

- `tools/package/compile_resources.py` 接入实际 `package_app.sh`：支持可选 `resources.json`、原始 INDEX8、RGB565 调色板、显式共享调色板的离线图片转换，并生成 `assets/resources.pxi`。
- 索引随资源一起进入现有签名清单；不另造签名或摘要权威。`test_package_tool.sh` 已将索引纳入实际签名、摘要与清单验证。
- `asset_catalog.h` 校验版本、规范路径、排序、文件成员关系、尺寸、偏移、存储/展开长度；文件 SHA-256 复用已签名 manifest。PNG/音频/普通数据也能进入索引，但并非所有消费者已经接入。
- `asset_loader.h` 实现 worker 使用的同步分块加载原语：32 字节头在栈上，每次读取最多 4096 字节，数据直接写入最终像素对象，无整文件临时副本；支持短读、分块取消、协作让出、校验失败回收。
- POSIX 文件适配器逐级 `openat` + `O_NOFOLLOW`，检查普通文件和实际大小，增量计算 SHA-256；覆盖叶节点和中间目录软链接、FIFO、路径穿越、缺失文件、同长度篡改。
- 实际跨语言链路已验证：Python 编译 → 文件索引 → C Host 校验 → 直接填充纹理对象 → 光栅器输出像素。仍未接入 Guest 的资源句柄与异步队列，不能将该测试当作应用端到端迁移已经完成。
- 协议与真实 Host API 说明：`deps/pxa-system/spec/draft/assets.md`。索引/文件格式明确为 1.0；未知版本拒绝。

此阶段验证：libpxa 15 项通过；资源索引/加载/所有权的 3 项 ASan/UBSan 测试通过；实际签名包测试通过；ESP 主机回归通过；两个模拟器入口构建，以及模拟器音频/渲染后端测试通过。日志分别为 `assets-regression.log`、`assets-asan-tests.log`、`assets-package-test.log`、`assets-esp-regression.log`、`assets-simulator-tests.log`。

## 应用迁移前构建基线

使用当前 voxel-craft 源码与已有 wamrc 2.4.3 重新构建，产物位于 `build/resource-refactor/baseline/{simulator,esp32s3}/pxa-voxel-craft`，大小及摘要记录在 `baseline/voxel-artifacts.json`。

| 项目 | 字节 |
| --- | ---: |
| main.wasm（两个目标相同） | 216364 |
| linux-x86_64 AOT | 374572 |
| ESP32-S3 AOT | 398464 |
| Wasm 声明的最小 / 最大线性内存 | 1179648 / 1572864 |
| Wasm 编码后 data section | 28082 |

表中是文件大小和静态声明，不是运行时 PSRAM/Guest 实际高水位。应用仍使用原来的资源上传方式，尚未迁移。新包已在 dummy SDL 视频/音频后端以 240×320 启动，出现 `voxel: raster surface ready`，运行 8 秒后由测试 timeout 结束（退出码 124）。`baseline/voxel-runtime.log` 保存主循环采样；这是启动/菜单冒烟测试，不能作为实际游戏场景的 FPS 基准。

## 已落地：有界缓存、异步 worker 与 Guest 资源服务

- `asset_cache.h/.c` 使用固定 workspace，提供全局/owner 两类内存预算、加载前预留、同包资源去重、独立请求取消、无持有者 LRU 淘汰和背压。真实释放完成后才归还额度；帧引用仍然阻止淘汰。缓存函数不执行 I/O、分配、释放或等待。
- POSIX worker 在真实文件上运行分块加载与校验。可注入每次读延迟和确定性 I/O gate；加载、最后释放均在 worker 上执行，运行时线程只做元数据与状态操作。
- 新增 Core Host-only 请求 identity，防止取消后复用 request_id 时，旧异步结果误命中新请求。使用运行时生命周期内不重复的 64 位序号，耗尽时拒绝分配。
- Assets service 21 / 1.0 实现 Core v1 QUERY 与异步 LOAD，预留终态事件容量；成功返回完整代际资源句柄，关闭和组件停止释放缓存 ticket。Guest SDK 提供查询、纹理/调色板加载、结果解析，取消和关闭复用 Core API。
- GameRender 0.5 增加批量绑定 direct I/O：一次校验全部句柄，支持显式解绑，失败保留整批旧绑定。desktop 和 ESP 的渲染后端均实现原子更新，旧帧继续持有旧对象。ESP 的资源文件 worker 和服务注册在下一阶段接入，见下文。
- desktop product runtime 已完成已验证包目录 → 索引 → worker → Assets service → WAMR Core v1 → Guest 句柄 → 实际渲染后端的链路。包需求校验、WAMR 服务入口、服务版本注册和 SDK 同步更新。

### 实际应用与像素验证

新增本地应用 `local/pxa-apps/resource-scenes`：20 张 256×256 源 PNG 离线转 INDEX8，共用 RGB565 调色板，每场景 4 张纹理，连续切换 100 次。Guest 不含像素/调色板数组，每次绑定后立即关闭 Guest 句柄，换场景显式解绑并提交低资源加载画面。

`pxsys_resources_test` 运行实际签名 AOT 和 product/WAMR 主循环，每次读取注入 1 ms 延迟，检查每个场景四张纹理的颜色和棋盘格采样点；应用完成后关闭上下文，测试驱动正常退出并走完整 Host 回收路径。已通过的日志为 `build/resource-refactor/service-app-test.log`，当前观察到：

| 项目 | 字节 / 次数 |
| --- | ---: |
| 外部类别纹理对象峰值 | 458976 B |
| 内部类别调色板对象峰值 | 544 B |
| worker/catalog/cache 固定元数据 | 16615 B |
| 配置的 native worker 栈大小 | 131072 B |
| 缓存淘汰 / 加载失败 | 393 / 0 |

512 KiB 仅为此次纹理对象预算，以上不是整个应用总 RAM。帧缓冲、Guest、AOT、线程库、OpenSSL 及分配器自身开销仍需完整汇总；不能由此宣称总内存优化验收已经完成。模拟器每包固定一个资源 worker，后续多应用 Host 集成必须保持总任务数有界。

此阶段回归：17 项 libpxa 测试通过；Assets service 和真实文件 worker 的 ASan/UBSan 通过；ESP 主机适配器回归通过；两个模拟器入口构建通过，desktop raster backend 测试通过。对应日志：`service-core-final-test.log`、`service-asan-test.log`、`service-esp-host-test.log`、`service-raster-test.log`。这些证据不替代最终两个 ESP 固件构建。

## 已落地：ESP 文件 worker 与 Host 生命周期

- `firmware/components/pxa/src/services/pxa_esp_assets.c` 使用固定的 FreeRTOS 资源任务；当前单活跃包 Host 的多次应用激活复用同一任务，不按纹理或请求增建任务。任务默认优先级 3、内部 RAM 栈 6144 B，索引与缓存 workspace 使用 PSRAM，纹理/调色板分别严格按对应预算类别分配，没有未记账的内存类别 fallback。
- 激活时从已验证包加载并校验有上限的索引；纹理/调色板通过 worker 分块读取，直接填入最终对象，增量 mbedTLS SHA-256 完整校验后发布。包目录必须是 Host 管理且不可修改的 LittleFS；该平台没有软链接/设备节点。不能把这一前提外推为任意 POSIX 文件系统安全保证。
- Assets 服务按应用声明惰性注册。worker 只设置原子标志并唤醒 Host，结果由运行时线程统一投递；没有每读取一个块生成一次 Guest 事件。
- 应用退出先关闭请求、句柄和渲染消费者，取消在途加载；`end()` 返回 `WOULD_BLOCK` 时 Host 等待排空，包清单和激活 arena 保留到 worker 完成。旧唤醒只指向永久 Host 状态，不指向已释放的应用对象。
- 每次读取最多 4096 B，在块之间检查取消和时间片。高优先级音频任务可抢占资源任务；资源任务每耗用约 2 ms 才主动休眠一个 RTOS tick，避免把每个短头部/数据块固定拖慢一整 tick。底层单次文件读取仍可能阻塞，尚不能据此宣称音频并发验收通过。
- 元数据统计包含退出后依然存在的静态任务状态和 mutex；配置栈大小单列。RTOS TCB、分配器、文件系统和 mbedTLS 的内部开销仍待完整计量，不能把激活 heap 清零描述成整个子系统零占用。

`pxa_esp_assets_backend_test.c` 编译实际 ESP 后端，使用 pthread 模拟 FreeRTOS 通知/锁、真实文件及系统 mbedTLS。100 次四纹理场景切换通过；同一任务跨激活复用，确定性测试覆盖读取中取消/重试/退出、帧持有导致退出延迟、同长度资源损坏与修复。模拟统计纹理对象峰值 458976 B、调色板 544 B、当前主机编译下元数据 16623 B；6144 B 是 ESP 配置栈，并非 pthread 的实际栈占用或栈高水位。测试也检查 100 Hz 假时钟下多个快读不逐块休眠、耗尽时间片后让出。

ESP 全套主机回归与新 worker 的 ASan/UBSan 通过。资源集成版本顺序构建 Pai Touch（ESP-IDF main / 6.2）与 Sensecap Watcher（5.5.4）通过：`esp-assets-{pai,sensecap}-build.log`。没有连接设备，这些是编译与模拟证据。

## 已落地：帧仅持有实际执行所需资源

- DrawList 验证遍历同时收集 48 位纹理集合与调色板依赖，不增加第二次命令扫描，也不改变 Guest 线格式。
- desktop 直接选择性保留引用；ESP 为并发安全先在短临界区取得完整校验快照，随后在锁外释放未使用引用，再发布帧。
- 清屏/平面填色不保留纹理；纯色精灵仍持有用于遮罩的纹理，但不保留调色板。带光照的 painter、覆盖和深度等模式按执行分支区分依赖。
- 所有既有光栅像素回归改为裁掉未声明依赖后执行，覆盖 sprite、批量、painter、深度、覆盖等路径。共享实际 ESP/desktop 场景还直接检查引用计数：排队清屏后解绑可以立刻释放旧资源，纯色精灵保留必要遮罩且画面正确。最高纹理槽位 slot 47 也有所有权检查。
- 17 项核心测试、7 项相关 sanitizer 测试、ESP 后端回归、两个模拟器入口构建、实际 desktop raster/audio 测试以及签名 AOT 的 100 场景慢 I/O 测试已通过。日志使用 `frame-dependencies-*` 前缀；包含帧依赖与时间片修正的代码已顺序构建 Pai Touch、Sensecap Watcher 成功，复验日志为 `frame-dependencies-{pai,sensecap}-build.log`。

### 本阶段性能复验

为避免把新校验器同时链接进旧基线而低估开销，基准增加 `PXSYS_RASTER_BASELINE_KERNEL`，可把开始时保存的 `raster.c` 与旧 `product_runner.c` 一起编译进旧目标。新目标使用当前代码。固定 CPU 8、Release、240×240、20 张驻留纹理/4 张可见、预热 200 帧、测量 3000 帧，交替新旧各 5 轮，原始数据与源码 SHA-256 位于 `frame-dependencies-saved-kernel-*`。

| 各轮中位数，微秒 | 旧基线 | 当前实现 |
| --- | ---: | ---: |
| 提交 p50 / p95 | 1 / 2 | 2 / 2 |
| 光栅化与呈现 p50 / p95 | 160 / 175 | 161 / 177 |
| 总时间 p50 / p95 | 162 / 176 | 163 / 179 |

每轮像素 checksum 均为 `dca27e35`。总时间 p50/p95 中位数分别变化约 +0.6% / +1.7%，未观察到可重复的超过 5% 回退。此基准检验 CPU 渲染开销与画面一致性，不衡量文件加载、完整应用内存收益或 ESP 帧率。

复现时在前面的 CMake 命令追加：

```sh
-DPXSYS_RASTER_BASELINE_KERNEL="$PWD/build/resource-refactor/baseline/raster.c"
```

## 已落地：voxel-craft 实际文件资源迁移

- 修改前保存应用源文件到 `baseline/voxel-source`，并运行原签名 AOT 捕获 48 个已绑定纹理、16×256 RGB565 调色板及完整菜单。原始生成器在主机构建时导出的数据，与原 AOT 上传的像素逐字节相同。
- 新增应用 `tools/generate_resources.py` / `export_resources.c`，离线运行现有程序纹理生成/量化代码，复用已生成的字体数据；不引入额外运行时图片解码器。51 个独立资源覆盖主路径及原来的低能力字体/纹理 fallback。
- Guest 主路径仅保留字体尺寸与渲染常量，移除运行时 RGB565/INDEX8 生成缓存、量化样本、光照调色板生成及上传 staging。发布配置仍为已有的 GameRender-only，CPU fallback 的生成器源代码保留但不会链接进默认 AOT。
- `voxel_assets.c` 使用现有真实 SDK 的异步加载、批量绑定和 Core 关闭/取消。一次只请求一个资源，绑定后立即关闭临时 Guest 句柄；全部准备好之前提交无资源加载画面，不绘制半完成的场景。后台取消当前请求并保留进度，恢复后重试；旧上下文的成功晚到只关闭句柄，不绑定到新上下文。加载失败最多重试三次。
- 默认 Host 缓存元数据条目从 32 增为 64，以容纳实际 48 张纹理加调色板；对象字节预算没有增加。desktop 与 ESP 均已调整。desktop 同时改为只给声明 Assets 的包创建服务和 worker，避免无关应用承担新线程/缓存成本。
- 新增 WAMR 运行时线性地址范围 current/peak 与仍保留的 Artifact 缓冲容量统计；实例化和每次 Guest 回调后采样。实际 WAMR 测试覆盖 `memory.grow` 后增长、组件退出后 current 清零而 peak 保留。桌面 mmap 可绕过普通分配器统计；ESP heap 路径又可能与其重叠，不能直接相加冒充物理 RAM。

### 实际测量与画面检查

`pxsys_voxel_resources_test` 运行实际签名旧/新 AOT，通过测试用固定 Clock NOW 保证菜单种子一致，帧调度仍使用真实时间。新资源读取每次注入 1 ms 延迟；48 张纹理、4096 个调色板颜色和完整 240×320 RGB565 菜单逐字节一致。可一条命令重跑对比：

```sh
python3 tools/compare-voxel-resources.py \
  build/resource-refactor/baseline/simulator/pxa-voxel-craft \
  build/resource-refactor/apps/pxa-voxel-craft \
  local/pxa-apps/.dev-signing/publisher-public.der
```

运行前按应用 `RESOURCES.md` 构建新包和 `pxsys_voxel_resources_test`。报告为 `build/resource-refactor/voxel-comparison/report.json`，其中包含实际运行快照、Artifact SHA-256、各资源与画面的 SHA-256；原始捕获也保留在该目录。

| 项目 | 旧上传路径 | 文件资源路径 |
| --- | ---: | ---: |
| main.wasm 文件 | 216364 B | 171836 B |
| Wasm 编码后 data section | 28082 B | 1714 B |
| Linux AOT / 此次实际保留的 Artifact buffer | 374572 B | 274380 B |
| ESP32-S3 AOT 文件 | 398464 B | 348668 B |
| Guest 实际线性地址范围 current / 观察到的 peak | 1167360 B | 1015808 B |
| Host 已绑定纹理与调色板对象 | 47444 B | 47444 B |
| 新资源 worker/cache/catalog 元数据 | 0 B | 32251 B |
| 新 native worker 配置栈容量 | 0 B | 131072 B |
| WAMR 分配器 current / peak（不含绕过它的 mmap） | 25277 / 98803 B | 25489 / 76591 B |

这证明 Guest 重复资源驻留和 AOT 已减少，当前显示内容没有变少。表中也明确列出新增 Host 开销，不把 151552 B 的 Guest 减少直接声称为整个应用净节省。该快照仅覆盖首个完整菜单及加载过程，完整的 gameplay、同时峰值、帧缓冲/栈/分配器/解码器开销汇总尚未完成；因此整个内存与性能验收仍然开放。

### 本阶段验证

- 真实 SDK 封装的应用加载器通过 ASan/UBSan：49/17 个绑定计划、后台暂停/恢复、取消、旧成功晚到、提交/加载/绑定失败；`voxel-resource-loading-test.log`。
- WAMR 实际运行回归与增长/退出统计通过；`voxel-wamr-test.log`。
- 原 100 场景签名 AOT 慢 I/O 测试再次通过，纹理峰值仍为 458976 B；64 条目配置下元数据为 30951 B，日志 `voxel-file-assets-scenes-test.log`。
- 两个模拟器入口构建、desktop raster/audio 测试、ESP 主机适配器回归通过。voxel-craft simulator 与 ESP32-S3 包均构建通过。
- 最后代码状态下 Pai Touch 与 Sensecap Watcher 固件已顺序构建通过：`voxel-file-assets-{pai,sensecap}-build.log`。没有硬件运行证据。

## 已落地：共享资源预算与首批后端接入

- `libpxa/resource_budget.h` 提供固定存储、两类内存、六类用途的共享预算。全局和 owner 额度独立检查；owner 使用完整 64 位代际身份。分配前在短锁内预留，实际分配/释放在锁外进行；包括分配头、分配尚未返回、实际 free 尚未返回以及扩容时新旧对象重叠。失败回滚，不存在未记账 fallback。
- `owner_close` 先禁止新分配；尚有对象时返回 `WOULD_BLOCK`，最后一份内存释放后才允许回收 owner。描述符保持稳定直到此时，避免旧对象通过新应用的 allocator 释放。当前 ESP 单活跃包 Host 的预算生命周期接在应用开始/退出边界；surface 元数据也计入预算，保证尚未结束的上传/呈现回调阻止预算身份提前回收。ESP 固定保留两组 allocator 描述符，让旧显示帧在退出后延迟释放，新应用可以启动且新旧对象共用全局额度；两组仍被占用时返回资源限制，不等待熄屏期间可能停止的显示任务。surface 捕获所属描述符，旧上传不能误用新应用的身份。
- 桌面与 ESP 的文件纹理、动态上传、UI 压缩文件及相关元数据均已接入。POSIX worker 增加可选的 Host 元数据 allocator，产品运行器会传入共享预算；独立适配器测试仍可使用普通分配器。ESP worker 的索引/cache workspace 同样计入。
- 动态 INDEX8 纹理在 ESP 使用 PSRAM，调色板使用内部 RAM，取消过去的内部 RAM 优先和无预算 fallback。相同用途的动态/文件对象共用全局额度，同时保留文件缓存自己的驻留/LRU 上限。
- 桌面短音效 PCM 缓存及路径已接入；ESP 音频、两平台第三方解码器内部内存、实际 LVGL 解码图片仍待接入，不能把此次额度描述成完整子系统内存上限。UI 的冷加载/解码仍需改成真正的异步工作路径。
- 默认共享额度为内部 RAM 128 KiB、外部 RAM 2 MiB。桌面可用 `PXA_RESOURCE_INTERNAL_BYTES` / `PXA_RESOURCE_EXTERNAL_BYTES` 注入；ESP 使用同名 `CONFIG_` Kconfig 项。文件缓存仍有自己的 64 KiB / 512 KiB 额度。这些值是配置上限，未预先分配等量内存，也不是依据真机性能调优后的结论。
- 此次原生 ABI 下固定预算计数器为 1680 B、12 个 allocator 描述符为 576 B，分配头为每块 16 B；已单列而不隐藏。这是 desktop 单组描述符的大小；ESP 为退役帧保留两组。本轮 Pai Touch ELF 中 budget=944 B、两组 activation/allocator=784 B、active/initialized/lock=16 B，符号大小合计 1744 B，不包含链接对齐空隙；提取结果为 `shared-budget-esp-budget-symbols.txt`。SDL mutex、native worker 栈、任务控制块、第三方库、Guest/AOT 和帧缓冲等仍单独统计，不能把 charged 数值与可能覆盖它的总堆统计直接相加。

### 预算验证

- Core 测试覆盖全局/应用互相独立的限额、按用途统计、扩容失败保持原数据、raw allocator 失败回滚、关闭时仍在分配、过期 owner、固定 owner 表耗尽及 6 线程共 120000 次竞争分配。raw allocator/free 回调内反查统计，以检查预留时机和锁外调用。18 项 Core 回归通过，5 项相关 ASan/UBSan 测试通过：`shared-budget-core-test.log` / `shared-budget-asan-test.log`。
- 新的 `pxsys_resource_budget_test` 调用实际模拟器分配入口，在 512 B 外部 RAM 额度下让文件对象、动态纹理、UI 压缩文件和短音效争用；验证拒绝、回收后重试、替换失败时旧纹理像素不变、关闭后禁止新分配以及最终归零。该测试读取的 PNG 仅用于压缩文件路径测试，不据此宣称真实 PNG 解码已经受预算约束。
- ESP 实际 worker 测试增加其他用途占满共享预算、文件加载失败回滚、释放后重试成功；既有 100 次场景切换、读取中取消、退出与实例替换继续通过。实际 surface 后端继续检查全部分配/释放在临界区外，并覆盖持有旧显示 lease 时退出、启动新应用后才释放旧帧，以及两组退役身份耗尽/回收。日志 `shared-budget-esp-host-test.log`；实际 worker 与 surface 后端的 ASan/UBSan 也通过：`shared-budget-esp-assets-asan.log` / `shared-budget-esp-surface-asan.log`。
- 实际签名 resource-scenes AOT 的 100 次切换继续通过：文件对象缓存峰值 458976 B，包含共享分配头和 worker/index 元数据后的外部 RAM 共享峰值为 490036 B，内部 RAM 峰值 560 B，退出后两类 charged 都为零。没有减少可见纹理数量或改变画质；日志 `shared-budget-scenes-test.log`。
- voxel-craft 旧/新 AOT 在新的 Host 上再次逐字节比较 48 张纹理、完整调色板和首个菜单，保持一致。`shared-budget-voxel-comparison` 保留本轮报告，不覆盖之前基线；快照同时输出共享预算 current/peak 和固定计数开销。依然不是整个应用的净内存收益结论。

- 两个模拟器入口构建通过。实际 desktop raster/audio 回归通过，日志 `shared-budget-{raster,audio}-test.log`。包含最后显示帧退役修正的 Sensecap Watcher、Pai Touch 固件顺序构建通过，分别为 `shared-budget-sensecap-build.log` / `shared-budget-pai-final-build.log`；没有实机运行证据。

### 本阶段光栅开销复测

新增 `tools/compare-raster-performance.py`，验证 Release 配置、旧 runner 与旧 raster kernel 均已指定，并记录源码/二进制 SHA-256、CPU 绑定、逐帧 CSV、每轮 p50/p95 和像素 checksum。新旧按轮交替运行；以各轮分位数的中位数比较，不将平均 FPS 当成回归判断。

CPU 8、240×240、20 张驻留/4 张可见的首轮 5×3000 帧测量中，总时间中位 p50 为 207→213 us，p95 为 239→259 us（+2.9% / +8.4%），脚本按阈值失败。原始异常保留在 `shared-budget-raster-comparison`，未删除。没有修改代码，扩大到 9×5000 帧复测后，p50 为 200.5→199 us、p95 为 226→225 us（-0.7% / -0.4%），位于 `shared-budget-raster-repeat`。两批 checksum 均为 `dca27e35`。

这次没有复现持续超过 5% 的整体回退，但个别轮次仍有明显长尾波动；当前机器使用动态频率并运行桌面程序，尚未把波动归因到具体因素。两批数据都保留，后续完整工作负载验收仍需检查。测试不包含文件 I/O、音频或 ESP 内存带宽。

```sh
cmake --build build/simulator/pai-touch -j8 --target pxsys_raster_bench_current pxsys_raster_bench_baseline
python3 tools/compare-raster-performance.py --cpu 8 --runs 9 --frames 5000
```

预算相关测试的重跑入口：

```sh
cmake --build build/resource-refactor/libpxa -j8
ctest --test-dir build/resource-refactor/libpxa --output-on-failure
bash firmware/components/pxa/tests/test_host.sh
cmake --build build/simulator/pai-touch -j8 --target pxsys_resource_budget_test
build/simulator/pai-touch/pxsys_resource_budget_test
```

## 已落地：共享压力回收与 ESP 音频缓冲记账

### 文件缓存响应其他资源的压力

- 共享预算增加可选的 `reclaim(context, owner, memory_class, needed_bytes)` 回调。额度不足时按真实缺口通知，底层分配器失败时按本次所需容量通知；均在预算锁外执行。不在回调中等待、读取文件或同步装载。连空预算都无法容纳的请求不会触发回收。
- Core `pxa_asset_cache_trim` 按内存类别选择无 ticket、无绑定/帧持有者的 LRU 对象，标记后由原有 worker 真正释放。重复压力请求会计入已经计划释放的对象；正在实际 free 的对象也不会重复计算为新候选。其他类别和有效消费者始终受到保护。
- POSIX/ESP 产品 Host 已把预算回调接到文件 worker。对已经接受的文件加载，如果共享分配失败且确有待释放候选，worker 先处理真实释放，再自动重试一次。仍然不足、没有可回收对象或等待队列已经满时，明确失败；不忙等、不无限重试，也不越过 `max_pending`。
- `pressure_retries` 与终态 `load_failures` 分别计数。这个机制只让文件缓存响应跨用途压力；尚未完成从文件加载侧主动回收 UI 和音效闲置缓存的全套协调，不应泛称所有缓存已统一淘汰。

实际签名 resource-scenes AOT 在 `PXA_RESOURCE_EXTERNAL_BYTES=300000`、每次读取注入 1 ms 延迟时，仍完成 100 次场景切换和全部可见像素检查。共享额度峰值 293804 B，内部 RAM 560 B，396 次有界压力重试、396 次淘汰、0 个最终加载失败，退出后 charged 为零。文件 cache 自己的峰值预留计数为 327840 B，包含被共享预算拒绝的尝试，不能把它误读成实际同时分配的内存。原始日志：`pressure-scenes-test.log`。

```sh
cmake --build build/simulator/pai-touch -j8 --target pxsys_resources_test
PXA_RESOURCE_EXTERNAL_BYTES=300000 build/simulator/pai-touch/pxsys_resources_test \
  build/resource-refactor/apps/pxa-resource-scenes \
  local/pxa-apps/.dev-signing/publisher-public.der
```

### ESP 音频的应用所有权

- 短 PCM 文件缓存经共享预算分配；音乐输出缓冲、重采样暂存及扩容也经同一入口。扩容仍先计入旧+新容量，最大输出块维持 65536 B，失败不会继续使用半完成结果。
- 音乐命令捕获所属应用的不可变分配器，并持有一个计入额度的小 pin。排队期间也会阻止旧分配器身份被提前回收；后台解码任务不查询“当前应用”的分配器。
- 覆盖排队命令前显式取出并释放旧 pin，停止时也移除尚未被 worker 接走的命令。过期命令、解码失败、停止和替换均释放其缓冲与 pin；即使旧解码任务延迟运行，也不能向新应用记账。
- 音效淘汰/STOP 的 heap free 已移出混音 mutex；渲染混音任务不进行新增的资源分配。解码器返回的 PCM 长度增加输出容量检查。
- 此处仍保留已有同步短音效冷加载行为；显式异步预加载与文件播放终态事件尚未完成。设备常驻 ring/cache 表、任务栈、队列及第三方 codec 内部分配尚未全部纳入共享额度，不能宣称音频总内存已经封顶。

### 本阶段验证与开销

- Core 回归 18 项通过；相关 5 项 ASan/UBSan 通过。最后新增的重试队列边界又通过了 cache、worker、budget 三项 sanitizer 复核：`pressure-core-test.log`、`pressure-asan-test.log`、`pressure-final-asan-test.log`。
- Core 缓存测试明确覆盖跨类别保留、ticket/frame 持有、重复回收通知、实际释放前仍记账、有界重试，以及等待队列在加载期间被填满的情况。
- ESP 实际文件 worker 测试覆盖其他用途填满预算、分配失败、自动回收无引用文件对象、释放后恢复。Host 全套回归与 worker sanitizer 通过：`pressure-audio-host-test.log` / `pressure-esp-assets-asan.log`。
- ESP 实际音频输出代码配合原生假 RTOS/codec 测试，覆盖缓存额度、命令覆盖/取消、旧新 owner 隔离、输出扩容成功与峰值不足、失败后归零；ASan/UBSan 通过：`pressure-audio-output-asan.log`。假 codec 仅用于确定性控制解码缓冲和生命周期，不替代 ESP 真实 Ogg 解码验证。
- 两个模拟器入口构建通过，desktop raster/audio 回归通过；voxel-craft 再次与旧 AOT 比较 48 张纹理、完整调色板和菜单像素一致。当前报告为 `pressure-voxel-comparison`。本轮未改光栅像素循环，没有新增实际 ESP FPS 结论。
- 包含最后队列边界修正的两个固件已顺序构建通过：`pressure-audio-sensecap-final-build.log` / `pressure-audio-pai-final-build.log`。当前没有设备，构建与模拟结果不能代替真机延迟、音频欠载或 PSRAM 带宽测试。
- 压力回调使 native budget 固定存储增加到 1696 B；新重试状态和统计令 64 条目 voxel worker 元数据变为 32779 B。本轮 Pai Touch ELF 的 budget/两组 activation/状态锁符号大小合计 1752 B，见 `pressure-budget-symbols.txt`。这些开销继续单列。

该阶段留下的 codec 调查已在下一阶段继续：必须检查实际选入对象，而非只看到 archive 有 WEAK 符号就宣称已覆盖。

## 已落地：ESP codec 分配入口接入共享预算

- `pxa_codec_memory.cc` 提供四个强定义：`media_lib_module_malloc`、`media_lib_module_calloc`、`media_lib_module_realloc`、`media_lib_free`。参数契约核对自 [Espressif media_lib_os.h](https://github.com/espressif/esp-adf-libs/blob/master/media_lib_sal/include/media_lib_os.h)。无需拦截整个固件的 libc malloc，也不额外引入 media_lib_sal OS 层。
- 任务局部 `PxaCodecMemoryScope` 绑定不可变 allocator。启动时注册 codec 使用设备 owner 的 EXTERNAL/METADATA；播放命令的 open/process/reset/get_info/close 使用捕获的应用 EXTERNAL/TEMPORARY allocator。scope 外分配失败；calloc 检查乘法溢出；realloc 保持旧+新峰值与失败回滚。free 不依赖当前 scope，仍归还原 owner。
- `pxa_esp_resource_memory_initialize` 可在设备启动时调用，后续应用 begin 不会重置预算。固定设备描述符与原两组应用/退役描述符共用全局限额；设备对象不妨碍已经排空的应用 owner 关闭。原生 surface 回归增加设备对象跨激活保留、重复初始化不重置计数、设备与应用竞争总额等检查。
- 固件增加强制 post-link `tools/audit-codec-memory.py`。当前审计基于 esp_audio_codec 2.5.0 的 ESP32-S3 两个 archive 摘要；遍历 map 中实际选入的 85 个 codec 对象，检查其分配 imports，并要求四个入口为强函数定义及预算 adapter 已被选入。内部清理/位分配函数继续遍历，直接 libc 分配、未知 media_lib 内存接口和弱 fallback 不允许。未选入的其他可选 codec 不据此宣称兼容。依赖更新、其他芯片或对象路径变化需要重新审查，不能直接更新摘要绕过检查。
- 首次审计构建把 `compute_allocation` 等内部函数误判为分配器，已修正为遍历这些辅助对象的最终 imports。失败日志保留为 `codec-budget-pai-build.log`；修正后 Pai Touch 构建通过，日志 `codec-budget-pai-final-build.log`。此检查不能证明第三方 codec 在任意坏输入/OOM 下都正确清理，也不能替代真实 ESP 播放测试。
- `pxa_codec_memory_test` 覆盖无 scope、calloc 溢出、限额拒绝、扩容回滚、嵌套 scope、关闭 owner 后跨线程延迟释放、两线程共 20000 次分配/扩容。实际音频输出配合 mock codec，新增经真实 hooks 申请私有内存及其额度不足场景；mock 测试不冒充真实 Ogg 解码。
- 已通过 hooks 与实际音频输出的 ASan/UBSan：`codec-memory-asan.log`、`codec-audio-output-asan.log`；设备/应用预算与实际 surface 生命周期的 sanitizer：`codec-device-budget-asan.log`。链接审计六项正负例见 `codec-audit-test.log`，包含通过内部 helper 间接调用原生 malloc 时拒绝。统一重跑入口仍是 `bash firmware/components/pxa/tests/test_host.sh`。
- 最后增加 codec 私有分配额度不足测试后，全套 Host 回归通过：`codec-budget-host-final-test.log`。Pai Touch 与 Sensecap Watcher 已顺序构建通过，分别为 `codec-budget-pai-final-build.log`、`codec-budget-sensecap-build.log`；两个构建目录均生成 `codec-memory-audit.json`，各覆盖 85 个对象，原弱 fallback 对象 `es_parse_mem.c.obj` 未被选入。
- Pai Touch ELF 的共享预算固定符号总计从 1752 B 增至 2144 B，新增设备 owner/allocator 为 392 B，见 `codec-budget-symbols.txt`。codec TLS 指针为 4 B，在 ESP-IDF 每个任务的已配置栈内占空间，另有 ABI 对齐影响；不应又把这 4 B 与完整栈分配重复相加。每个 codec heap 块只使用已有预算分配头。

该阶段结束时设备 ring/cache、队列/任务控制块/栈仍待接入，下一阶段已落实如下。UI 实际解码、desktop codec 和临时空间专项上限仍未完成。实际 ESP codec 私有峰值与 OOM 清理行为没有真机运行证据。本阶段没有修改光栅热循环，也没有新的 FPS 或全系统净内存收益结论。

## 已落地：ESP 音频设备常驻内存与初始化清理

- `PxaAudioOutput` 的 8192 样本 ring、8 条短音效缓存元数据表、两个任务栈/TCB、两条队列的控制块/载荷、mutex 均通过设备 owner 记入共享额度。RTOS 存储严格使用内部 RAM；ring 和缓存表使用 PSRAM，无静默 fallback。
- 使用 `xQueueCreateStatic`、`xSemaphoreCreateMutexStatic`、`xTaskCreateStatic`，任务数量和优先级不变。全部预算分配及 codec 注册成功后才启动两个任务，避免动态创建第二个任务失败时清理已经运行的第一个任务。合法的非空静态存储不会因 heap 不足而创建失败；若 RTOS 契约被破坏，断言失败而非尝试释放运行中任务的 backing memory。
- 20 KiB 解码栈单独申请，其余 RTOS 存储合并，未把最大连续请求扩大为接近 28 KiB。初始化失败撤销之前成功的 Vorbis/Opus 注册，清理队列、mutex 和全部已申请块，并恢复可重试状态；重复成功初始化不重复注册或创建任务。
- 设备 owner 不随应用退出关闭。测试明确区分应用额度归零与设备常驻量，旧应用不能释放设备存储，新应用可用的全局额度会扣除常驻内存。析构/停机不属于这里新增的应用 API，板级音频对象和任务仍保持设备生命周期。

从两个板子的最终音频编译对象 DWARF 类型读取（不是真机 heap 测量）：

| 常驻项 | 载荷 / 类型大小 | 预算计费 |
| --- | ---: | ---: |
| RTOS 合并存储，含 4 KiB 输出栈 | 7484 B | 7492 B |
| 音乐解码栈 | 20480 B | 20488 B |
| 8192 × int16 ring | 16384 B | 16392 B |
| 8 × SoundAsset 缓存表 | 4224 B | 4232 B |

以上内部 RAM 合计 27980 B，PSRAM 合计 20624 B，已包含每个分配块 8 B 的共享预算前缀。codec 注册对象另经设备 METADATA 计费，不能从这个表推断完整 codec 占用。`PxaAudioOutput` 内联对象为 172 B，包含新增的两个指针；它属于板级对象本体，不能与包含该本体的板级 heap 分配重复相加。RTOS 栈中的 TLS 和控制块字段已经包含在表内。证据为 `audio-resident-{pai,sensecap}-sizes.txt`；两个 IDF 版本本次读到的类型大小相同。此阶段主要补齐预算覆盖和失败路径，不把这些原本存在的字节宣传为新增内存节省。

测试新增：七处分配逐一注入失败（四个设备块及 mock codec 的三个注册块）、内部/外部全局额度分别耗尽、失败后重试、重复初始化、仅准备完整后才创建任务、设备与两代应用隔离、应用退出后设备常驻量不变。假 RTOS 使用调用者提供的静态控制块；mock codec 仍只用于确定性验证分配与清理，未模拟实际 ESP 解码速度。

- Host 回归通过：`audio-resident-host-final-test.log`。
- 实际音频输出 ASan/UBSan 通过：`audio-resident-output-asan.log`。
- 完整 ESP Host 适配器测试首次统一通过可选 sanitizer 模式：`audio-resident-host-asan.log`，包括实际文件 worker/surface 生命周期、音频输出和 codec 内存 scope。Core 静态库也以 sanitizer 编译。
- Pai Touch 最终固件通过：`audio-resident-pai-final-build.log`，codec 链接审计仍覆盖 85 个对象。
- 随后的 Sensecap Watcher 固件也通过：`audio-resident-sensecap-build.log`，相同 codec 审计通过。两板顺序构建，没有烧录或真机运行。实际音频对象的分配/创建 imports 保留于 `audio-resident-allocation-imports.txt`，RTOS 创建均走静态接口。

```sh
bash firmware/components/pxa/tests/test_host.sh
PXA_TEST_SANITIZERS=1 bash firmware/components/pxa/tests/test_host.sh
```

完整内存上限仍未完成：UI 真正解码、桌面 codec、资源 worker 栈、显示/驱动及其他平台分配需要继续覆盖或单列汇总；临时空间专项限额、异步音效/UI 和综合生命周期/性能验收继续保留在原目标中。

## 后续必须完成的工作

本轮新增的预取/状态接口和实测见下方阶段记录；以下清单仍保留完整原目标。

1. 两平台文件纹理链路已经实现；继续补 Host 级异常、后台/恢复与并发播放验证，不能以 worker 单测代替这些集成场景。
2. 共享预算已接通两平台文件/动态纹理、UI 压缩数据与元数据，以及桌面短音效。ESP 的短音效、音乐输出/重采样缓冲、音频设备常驻 ring/cache/RTOS 存储与当前链接 codec 的分配入口也已接通；临时空间专项限额已落实，见文末。继续接入 UI 实际解码、desktop codec 内部并汇总其他任务/显示/驱动内存，补真实解码行为验证。文件缓存已有共享压力回收/有界重试，反向回收 UI/音效闲置缓存及固定开销汇总仍待完成。当前额度仍不覆盖整个子系统。
3. 显式预取/状态契约已打通 SDK、WAMR、Core 和两个后端；场景 SDK 封装也已完成，见文末验证。继续完成有界 Guest 数据块读取及后台预取政策的完整集成验收。帧的选择性资源持有已经实现。
4. voxel-craft 文件资源迁移已完成首条实际路径；继续验证 gameplay、重建/后台恢复与预算失败。resource-scenes 接入短音效预加载和背景音乐，同时验证纹理 I/O 不饿死音频。UI 仅在表示兼容时共享对象，转换成本必须统计。
5. 在真实应用服务路径补全缺失/损坏文件、低预算恢复、队列满、后台/锁屏/恢复、加载中退出和实例替换；当前 Core/worker 测试已覆盖部分，但不替代完整集成验证。
6. 记录同画质的全量内存、冷/热加载、CPU 渲染与帧时间分布、音频欠载及原始数据，完成真实应用或代表性资源基准的前后对比。
7. 顺序完成 Pai Touch、Sensecap Watcher 固件构建，更新 API/迁移/打包文档和最终报告，逐条审计原目标后才可声明完成。

## 已落地：Assets 1.1 预取与状态查询

- 新增 PREFETCH(3) 与 STATUS(4)，请求沿用有界路径报文；更新服务版本、协议注册表、打包默认需求、WAMR 的完整 token 转发和 Guest SDK。Host backend 增加 prefetch/inspect 回调，POSIX 与 ESP 都接入实际文件缓存。
- 预取沿用 Core 请求和缓存 ticket，没有第二套调度器。成功结果只返回 20 字节 metadata；发布结果后释放 ticket，不占 Guest 资源句柄表，缓存可淘汰。错误和取消只返回状态。测试覆盖句柄表满时仍可完成预取，以及请求取消和完成后所有 ticket 归零。
- 队列优先处理仍有普通加载订阅者的任务，再按队列年龄处理预取。合并请求提升优先级，取消最后一个普通订阅者后恢复预取优先级。正在运行的读任务不被抢占；队列满仍返回背压。缓存请求的 active 字段区分两种订阅，foreground 计数受固定 ticket 表大小约束，没有额外线程或动态请求对象。
- STATUS 是当前组件/包/摘要/表示范围内的快照，支持 ABSENT/QUEUED/LOADING/READY/FAILED；不读文件、不 pin、不触碰 LRU。索引路径不存在与索引资源当前未驻留分别返回 NOT_FOUND 和成功的 ABSENT。旧失败 ticket 与重试并存时优先报告重试/已就绪条目。取消是请求终态，不是共享缓存的终态。
- SDK 提供 `pxa_assets_prefetch_texture/palette`、`pxa_assets_status`、`pxa_assets_parse_status`；预取通过 `pxa_assets_parse_result` 解析，以 `pxa_cancel` 取消。无需手写协议。`spec/draft/assets.md` 记录 wire 格式、版本和所有权；完整场景辅助封装仍未交付。
- `resource-scenes` 的实际应用现先查询调色板状态，再加载；每个可见场景可选预取下一场景第一张纹理，共 99 次。预取失败不阻断前台场景；换场景仍卸下旧绑定、画加载帧、LOAD 并批量绑定。停止时取消预取。它仍未包含目标要求的并发音乐/音效示例，不能视为完整验收。

### 原生与实际应用验证

- 18 项 Core 回归通过：`prefetch-core-final-test.log`。新增缓存测试验证优先顺序、合并/降级、取消、失败与重试状态、无持有的回收；服务测试通过实际 C SDK 报文验证结果解析、取消、满句柄表及状态保留位校验。
- 三项相关 ASan/UBSan 通过：`prefetch-asan-final-test.log`（cache、service、真实 POSIX worker）。完整 ESP Host sanitizer 通过：`prefetch-host-asan.log`，实际 ESP worker 增加预取及组件隔离状态查询。
- 协议校验和生成可复现通过：`prefetch-spec-full-test.log`；完整打包测试通过：`prefetch-package-tool-test.log`，默认版本测试包含 Assets 1.1。一次直接调用需要 fixture 参数的版本子测试命令不完整，保留于 `prefetch-package-version-test.log`；随后使用包含正确 fixture 的完整打包脚本验证。
- 实际签名 resource-scenes AOT，经 WAMR 和 desktop product loop，在每次文件读取注入 1 ms 延迟时完成两组各 100 次切换，验证每场景四张纹理像素及正常退出。新断言要求 99 个预取请求，并单独核对前台与可选预取失败，不把所有失败清零或忽略。

| 条件 | 默认共享额度 | 外部共享额度 300000 B |
| --- | ---: | ---: |
| 预取请求 | 99 | 99 |
| cache hit | 99 | 0 |
| 可选预取失败 | 0 | 99 |
| 必需加载失败 | 0 | 0 |
| 淘汰 | 393 | 396 |
| 共享压力重试 | 0 | 396 |
| 外部共享峰值 | 490572 B | 293820 B |
| 内部共享峰值 | 560 B | 560 B |
| 退出后的两类 charged | 0 | 0 |

日志：`prefetch-scenes-test.log`、`prefetch-pressure-scenes-test.log`。统计新增 `prefetch_requests` 与 `prefetch_failures`，后者是没有普通加载订阅者时失败的 `load_failures` 子集；因此低预算下总 load_failures=99 不代表前台失败。低预算 cache 自身峰值预留仍可能大于实际全局分配，包含被拒绝的尝试，不能当作实际 RAM 占用。该阶段 POSIX 场景 fixture 的 worker metadata 为 31420 B、ESP fixture 为 16903 B；增加的两个统计计数器为 16 B。

```sh
cmake --build build/simulator/pai-touch -j8 --target pxsys_resources_test
build/simulator/pai-touch/pxsys_resources_test \
  build/resource-refactor/apps/pxa-resource-scenes local/pxa-apps/.dev-signing/publisher-public.der
PXA_RESOURCE_EXTERNAL_BYTES=300000 build/simulator/pai-touch/pxsys_resources_test \
  build/resource-refactor/apps/pxa-resource-scenes local/pxa-apps/.dev-signing/publisher-public.der
```

两个模拟器入口构建通过：`prefetch-{pai,sensecap}-simulator-build.log`，desktop 实际预算路径回归通过：`prefetch-desktop-budget-test.log`。Pai Touch 含 WAMR 新 opcode 转发的最终固件通过：`prefetch-pai-final-build.log`。本阶段没有改变像素循环，没有测出或宣称新的实际 ESP FPS；冷/热加载延迟及同画质 frame p50/p95 仍需最终综合验收。

随后 Sensecap Watcher 顺序固件构建也通过：`prefetch-sensecap-build.log`；两个最终固件均通过 85 个 codec 对象的分配链接审计。本阶段没有使用 ESP 实机。

## 已落地：有界场景 Guest SDK 与加载中退出

- 新增 `sdk/guest-c/include/pxa_asset_scene.h`，提供调用者持有的描述、绑定数组和场景状态，预先检查整组描述，以有界窗口调用现有 LOAD，并由应用原来的事件分发器推动。复用完整 64 位 token、Core 取消/关闭及 GameRender 原子批量绑定；没有新增堆分配、Host API、事件循环、像素缓存或 worker。
- `begin` 的后续提交若被拒绝，之前已接受请求仍需排空；`release` 取消本组请求并关闭其 Guest 句柄。取消输给已排队的成功时，晚到句柄仍被关闭。未排空请求或未成功关闭的句柄阻止下一次 begin，失败保留首个原因；没有隐式无限重试。绑定失败保留旧绑定及新句柄，成功绑定后释放 Guest 句柄不会撤销绑定/在途帧的独立引用。
- 请求窗口不等于整组句柄上限。描述最多 49 项，但默认 Host 资源额度仍为 32，不能靠窗口 1 假装同时持有 49 项。voxel-craft 已有的渐进绑定路径保留，未为追求统一封装增加其峰值。项目/路径借用到请求排空，绑定数组在关联 scene 期间始终有效；解除关联需成功 release、排空、再清零 scene。
- `resource-scenes` 已迁移四纹理一组的加载、结果处理、批量绑定及失败清理，继续显式卸下旧绑定、画加载帧并做有次数上限的重试。修正 stop 示例：Core 在回调前撤销资源，回调禁止 imports，因此不能在 stop 内调用 release/cancel/close。正常 start/event 回调才做显式释放和排空。
- SDK README、协议所有权说明、根设计文档及应用 README 同步更新，附真实 API 示例及生命周期限制。此封装完成并不代表并发音乐/音效、UI 异步解码、有界 Guest blob 读取或完整生命周期矩阵已经完成。

### 验证与开销

- 19 项 Core 回归通过：`scene-sdk-core-test.log`。新增独立 SDK 测试覆盖有界窗口、乱序结果、完整 token、重复结果、原子绑定失败、立即拒绝、异步失败、关闭失败保留所有权、取消输给成功及禁止未排空复用。实际 Core Assets 服务测试也验证成功已排队后取消与下一次加载。
- 两项相关 ASan/UBSan 通过：`scene-sdk-asan-test.log`，包含 SDK 状态机与真实 Core 服务。
- 真实签名 AOT 默认预算和外部共享预算 300000 B 各完成 100 个场景，每场景四张纹理像素验证通过；99 次可选预取，前台加载失败均为零。两组外部峰值仍分别为 490572 B、293820 B，内部均为 560 B，退出两类 charged 均为零。低预算预取失败 99、压力重试 396，与迁移前相同：`scene-sdk-scenes-test.log`、`scene-sdk-pressure-scenes-test.log`。
- 新增 `pxsys_resources_test ... exit-loading`，每次文件读默认注入 20 ms 延迟，观察加载帧后实际外部纹理正在加载且请求未结束时退出，而不是等场景完成。签名 AOT 正常停止、worker 排空、两类预算归零通过：`scene-sdk-exit-loading-test.log`。此退出样例仍不替代后台/锁屏/恢复和跨实例 ABA 的完整应用验证。
- 当前应用 simulator 与 ESP32-S3 Guest AOT 构建通过：`scene-sdk-package.log`、`scene-sdk-esp-package.log`。本阶段不改 Host 生产 ABI/backend，未重做两个完整固件；前阶段两板最终构建记录保留。Guest AOT 编译不等于 ESP 真机执行。
- 当前与迁移前留存产物 `scene-sdk-baseline` 比较：Wasm 8951 → 9898 B（+947 B），Linux AOT 17548 → 16736 B（−812 B）。记录为 `scene-sdk-size-comparison.json`，并非所有平台统一缩小的结论。
- 使用 clang wasm32 布局实测：scene 40 B，4 项 binding 64 B，4 项 item 32 B，4 条路径 60 B，共 196 B；原先 handle/token 两数组共 64 B。此比较不包含其余应用变量及栈，也不等于完整 Guest 高水位变化。结果 `scene-sdk-wasm32-sizes.ll`。begin 与 bind 各有最大 592 B 报文 scratch；嵌套调用栈另算，不能把单个局部数组冒充整个峰值。

重跑当前实际应用退出验证：

```sh
cmake --build build/simulator/pai-touch -j8 --target pxsys_resources_test
build/simulator/pai-touch/pxsys_resources_test \
  build/resource-refactor/apps/pxa-resource-scenes \
  local/pxa-apps/.dev-signing/publisher-public.der exit-loading
```

完整目标仍以原契约为准：后续继续有界 Guest 数据读取、UI 冷加载/解码、短 PCM 异步预加载与完整文件播放通知、同时纹理/音乐/音效演示、临时工作区专项硬限额与全量内存核算、生命周期/异常矩阵，以及最终同画质性能对比和两板顺序构建。不能据本阶段通过就宣布整体完成。

## 已落地：临时解码内存专项硬限额

- `resource_budget` 新增两类内存的 `temporary_limit`，所有 owner 的 TEMPORARY 分配同时受专项额度、全局总额度和应用总额度约束。零明确禁止该类别分配，Host 初始化时必须填写；未加运行时可变策略或第二套分配器。前缀、原生分配尚未完成的预留、扩容旧+新和退役 owner 的活块均计入，真实 free 返回后才返还额度。
- 复用已有 by_kind 计数检查上限，另外保存 global/owner 两类 temporary_peak，能够区分解码峰值与常驻资源峰值。专项不足不触发纹理回收，因为释放 raster 不会减少 temporary 计数；仍立即失败，由消费者决定重试或终止。原生 heap 分配失败及总额度压力继续使用原有回收契约。
- ESP 与 desktop 默认内部 16 KiB、外部 512 KiB，可分别通过 Kconfig / `PXA_RESOURCE_TEMPORARY_{INTERNAL,EXTERNAL}_BYTES` 调整。总额度小于专项额度仍由总额度限制。desktop 数值设置现在允许明确的零。上限不是物理预留或实际解码峰值，也不把栈/未捕获的第三方分配自动纳入统计。
- 没有修改 Guest ABI、事件数量、光栅像素循环或混音热路径。ESP 现有输出/转换缓冲及 codec hooks 因使用 TEMPORARY 自动受限；设备常驻环形缓冲、RTOS 和 metadata 仍按原类别记账。

### 验证

- 19 项 Core 回归通过：`temporary-budget-core-test.log`；budget/cache/service 三项 ASan/UBSan 通过：`temporary-budget-asan-test.log`。
- 预算测试新增跨两个 owner 的专项竞争、退役 owner 继续计费、零类别拒绝、专项满时其他驻留类别仍可分配、扩容失败保留原块、原生失败回滚，以及专项下六线程共 120000 次竞争；与原总预算测试合计 240000 次。确认专项拒绝不调用无效的缓存回收。
- 完整 ESP Host ASan/UBSan 通过：`temporary-budget-host-asan-final.log`。实际 ESP allocator 验证设备/应用共享专项额度；真实 codec 分配 hooks 验证跨 scope 限额；实际音频输出代码配合 mock codec，在总额度足够而专项仅剩 25000 / 21000 B 时分别验证输出扩容/codec 私有分配失败、部分块回收、释放占用后重新成功播放。mock 不代表真实 ESP codec 的速度、声音质量或其所有 OOM 清理路径。
- 原 ESP worker 压力 fixture 曾使用 TEMPORARY 填满几乎整个共享总额，新限额正确阻止了它。该测试现改用常驻 IMAGE 分配制造总额压力，保留原失败/回收/恢复覆盖，并由新增用例单独验证专项行为。初次失败保留于 `temporary-budget-host-asan-initial.log`。
- 两个模拟器入口构建通过：`temporary-budget-{pai,sensecap}-simulator-build.log`。实际共享预算、raster 及 Vorbis/Opus 音频后端测试通过：`temporary-budget-desktop-test.log`、`temporary-budget-raster-test.log`、`temporary-budget-audio-test.log`。第一次直接运行音频测试遗漏 fixture 路径，退出在 argc 断言；原日志 `temporary-budget-audio-test-initial.log` 保留，随后带正确的 `tests/audio-assets` 参数验证。
- 实际签名 resource-scenes 在默认/300000 B 外部总额度下各完成 100 次场景切换，前台失败为零、像素校验通过；外部峰值仍为 490572 / 293820 B，内部 560 B，退出 charged 均归零。加载中退出也通过。日志为 `temporary-budget-scenes-test.log`、`temporary-budget-pressure-scenes-test.log`、`temporary-budget-exit-loading-test.log`。这些场景的 temporary_peak 为零，因为纹理直接加载进最终对象；这不代表未计入的 native crypto/栈无工作区。
- Pai Touch 固件通过，85 个 codec 对象分配审计通过：`temporary-budget-pai-build.log`。从最终 ELF 读取 stats/config/budget 分别为 96 / 36 / 1032 B，ESP 共享预算相关固定存储合计 2224 B，比上阶段增加 80 B；native budget 固定存储从 1696 增至 1856 B（+160 B）。新统计成本没有计作节省。类型证据 `temporary-budget-pai-sizes.log`。

本阶段仍不证明 UI/desktop codec 全部分配已受控、完整音频异步预加载和终态事件已实现，或全系统净内存收益已达标。继续保留有界 Guest blob 读取、UI 冷加载与解码、音效预加载/播放通知、同时纹理/音乐/音效示例、完整生命周期矩阵与同画质最终对比等原目标。

Sensecap Watcher 随后顺序构建通过：`temporary-budget-sensecap-build.log`，85 个 codec 对象分配审计通过。最终 ELF 的 stats/config/budget 类型及共享预算固定存储与 Pai Touch 相同，见 `temporary-budget-sensecap-sizes.log`；两板生成配置均为临时内部 16384 B、外部 524288 B。本阶段无 ESP 设备运行证据。

## 已落地：blob 分块认证与读取基础

这是有界 Guest 数据读取的基础阶段，尚未交付 Guest READ/SDK 或两平台异步读取链路，不能将下列 Host 原语当成应用 API。

- 编译器生成 PXRI 1.1：raw blob 每 4096 B 一个 SHA-256，紧跟该记录的补齐路径；数量从 signed stored_bytes 推导，末块只对实际字节计算摘要，空文件不附摘要。索引本身仍由现有 manifest 签名及 inventory SHA-256 认证，不增加签名方、whole-file 摘要权威或整文件副本。
- 当前 catalog 同时接受 1.0/1.1，未知版本拒绝。1.0 仍可用于纹理与 metadata，blob 部分认证明确 UNSUPPORTED，需重新打包；PXR1 保持 1.0。新表的截断、溢出/巨大声明、尾部多余数据与原路径/文件成员校验均在 catalog 初始化时检查。
- `pxa_asset_catalog_blob_block` 无分配/IO 地返回 offset 所在块、完整块长度及借用的摘要；offset==EOF 返回空结果，越界/错误 kind 拒绝。没有向 `pxa_asset_info_t` 或每个 raster cache entry 增加指针/字段，避免让所有纹理都支付 blob 元数据成本。
- `pxa_asset_load_blob_block` 使用调用者提供的 Host 缓冲，最多读取/校验一个 4 KiB 块，支持短读、分块取消、末块及失败结果不发布。读取内块时不额外探测文件 EOF。
- POSIX `pxa_posix_asset_open_range` 沿用安全目录逐层 openat/O_NOFOLLOW、普通文件及完整 signed 文件长度检查，然后 seek 到目标范围，仅累计该范围的摘要。不为读取地图的一小段而反复扫描整文件。目标块之外的篡改在读取相应块时检测，不将一次读块描述成全文件重新验证。
- 索引增加每块 32 B，整块约 0.78%，末块向上取整，继续受 Host max_catalog_bytes 限制。例如 1 MiB blob 增加 8192 B 索引；这是明确成本，不能声称文件化后没有 metadata 常驻。读取结果缓冲与异步排队预算将在接入 worker 时记录。

### 验证与当前边界

- Core 19 项通过：`blob-block-core-test.log`；增加 compiler 的多块/空文件/确定性测试，以及 catalog 的旧版本拒绝读块、未来版本、缺少摘要、巨大尺寸、EOF、短读和取消等用例。新增巨大声明用例的最终普通/sanitizer 复核：`blob-block-catalog-final-{test,asan-test}.log`。
- 5 项相关 ASan/UBSan 通过：`blob-block-asan-test.log`，含新的跨 Python 编译器与 POSIX 真文件 `pxa_asset_blob_test`，验证每块数据、非对齐查询、首/中/末块、空文件、EOF、取消、同长度篡改、文件截断、缺失和软链接拒绝。fixture manifest 由测试提供；实际签名验证由完整 package suite 单独覆盖，不冒充 Guest AOT 读 blob 已通过。
- 完整包测试通过：`blob-block-package-test.log`；完整 ESP Host sanitizer 通过：`blob-block-esp-host-asan.log`。后者证明新 catalog 不破坏已有 ESP worker/音频/生命周期路径，尚不证明 ESP 异步 blob 读取。
- 两个模拟器入口构建通过：`blob-block-{pai,sensecap}-simulator-build.log`。重新签名的 PXRI 1.1 resource-scenes 实际 AOT 连续切换 100 场景，逐场景四纹理像素通过、前台加载失败零、退出 charged 零：`blob-block-scenes-package.log` / `blob-block-scenes-test.log`。内部/外部共享峰值 560 / 490572 B，worker metadata 31420 B，与该样例前阶段相同；该样例没有 blob，因此不把它当作 blob 内存/性能验证。
- Pai Touch 固件构建通过：`blob-block-pai-build.log`，codec 链接审计仍通过。本阶段未进行 ESP 设备运行。

重跑底层块读取验证：

```sh
cmake --build build/resource-refactor/asan -j8 --target pxa_asset_blob_test pxa_asset_catalog_test
ctest --test-dir build/resource-refactor/asan --output-on-failure \
  -R 'pxa_asset_(blob|catalog)_test|pxa_resource_compiler_test'
```

下一步把上述认证块接入现有资源 worker：有界请求与结果缓冲、临时预算、与纹理工作公平调度、Core 预留完成事件、代际/取消/退出排空、运行时复制到 Guest，以及明确借用期的 SDK 结果解析。计划中的 READ 按路径/offset/最大长度请求，结果可以在 4 KiB 边界返回部分数据并携带总长度；worker 不保留 Guest 原生指针。最终需要实际签名应用读地图并覆盖两平台同语义，不能停在本阶段原语。

补充空文件/EOF 的取消语义后，相关两项 ASan/UBSan 通过 `blob-block-final-asan-test.log`，Core 19 项最终回归通过 `blob-block-final-core-test.log`。两个模拟器入口重新构建通过 `blob-block-final-{pai,sensecap}-simulator-build.log`。Sensecap Watcher 包含该修正的最终顺序构建通过 `blob-block-sensecap-final-build.log`；此前两个初始固件构建不冒充覆盖这一最后修正。

Pai Touch 随后最终构建也通过：`blob-block-pai-final-build.log`。两板最后的 asset_loader 对象均在 EOF 取消修正后重编译，记录为 `blob-block-final-object-check.json`；两个最终链接的 85 对象 codec 内存审计仍通过。Guest 读取链路仍待下一阶段完成，整体目标没有标记完成。

## 已落地：Assets 1.2 有界 Guest 数据读取

- 新增 READ opcode 5：SDK 接受路径、offset、最大长度与完整 64 位 token，Core 沿用预留完成事件、取消和请求身份校验。成功返回偏移、总长度与借用到当前事件结束的数据视图，不产生资源句柄，不把 Guest 指针交给 worker。只允许 PXRI 1.1 认证的原始 blob，纹理/音频保持 Host 消费路径。
- 单次有效数据上限为 **4064 B**，与 20 B v1 信封、4 B 状态和 8 B 结果头合计 4096 B。此前开发中的 4096 B 数据上限无法装入现有控制消息，已修正；认证块仍为 4096 B。读到块边界可以短读，EOF 返回零字节，越界返回错误。
- POSIX 和 ESP 在原有资源 worker 中增加固定四槽读取队列，交替服务读取与纹理任务；无新线程。每个任务只有一块预算内认证缓冲，验证后在同一块内整理片段，短结果不会缩小实际分配。Core 复制到预留事件后释放 ticket；worker 在锁外真正释放，随后归还槽位/预算。取消、关闭和应用退役都等待读取排空。
- WAMR 原先统一按 v1 增加 12 B 预留事件缓冲，会使合法的 4096 B READ 完成扩容到 8192 B。现在按信封实际增加的 8 B 预留；Store progress 的 4 B 载荷扩展仍可装入最小 64 B 缓冲，已有相关回归通过。内存快照新增 `event_buffer_bytes`，明确它已经包含在线性内存中，不能重复相加。
- `resource-scenes` 增加包内 8193 B 地图及可复现生成脚本；Guest 逐事件验证整个文件、短读和 EOF，使用高于 UINT32_MAX 的 token，与纹理加载共用 worker。没有地图大小的 Guest 数组。产品测试增加 `exit-reading`，在 TEMPORARY 认证缓冲已分配且注入 I/O 延迟时退出，验证后台排空和预算归零。

### 验证与实际开销

- Core 20 项、ASan/UBSan 25 项、WAMR 26 项回归通过：`blob-read-core-test.log`、`blob-read-final-asan-test.log`、`blob-read-wamr-test.log`。Core 新测试覆盖事件容量不足时不启动 I/O、错误元数据/类型/路径、坏结果、Core ID 复用、取消输给已排队完成和 stop。独立队列测试覆盖 FIFO、四槽满、实际释放前不可复用、临时压力/恢复、取消、认证失败、EOF 与序列耗尽。
- 真实 POSIX/ESP worker 共用一套文件行为检查，覆盖实际块摘要、跨块短读、队列满、其他 owner 无法读取/取消、阻塞中取消并重试、损坏块局部拒绝/修复以及临时额度耗尽/恢复。ESP 主机 ASan 完整通过：`blob-read-final-esp-host-asan.log`；额外覆盖 blob 加载中退役、重复激活、原永久任务复用及零残留。
- 签名 AOT 默认和共享外部预算 300000 B 分别通过地图读取及 100 次场景像素检查：`blob-read-final-scenes-test.log`、`blob-read-final-scenes-low-test.log`。最大 Guest 事件缓冲实测 **4096 B**；本例顺序读取 TEMPORARY 峰值 **4120 B**（4096 B 校验块 + 8 B 头 + native 16 B allocator prefix）。四个同时保留的结果理论可达此值四倍，仍需同时满足全局/应用/TEMPORARY 额度。
- 默认共享预算外部峰值 491220 B，低预算峰值 298588 B，内部峰值均为 560 B，退出 charged 均为零。低预算仍有 99 次可选预取失败与 396 次压力重试，前台纹理成功。这些仅为接入共享预算的资源统计，不是整机/进程总内存；事件池、Guest 线性内存及尚未接入的开销必须另列。不能将短读发布后释放 worker 缓冲称为 Guest 线性内存缩小。
- 新场景应用 wasm 为 10666 B、Linux AOT 18228 B，索引由 852 B 增至 988 B，地图 8193 B 单独存储。增加读取功能有固定代码、队列和事件缓冲成本，本阶段不宣称场景基准内存比未执行地图读取时下降。数据汇总：`blob-read-measurements.json`；相关源码摘要：`blob-read-source-identity.json`。
- 两种加载中退出均通过：`blob-read-final-exit-reading-test.log`、`blob-read-final-exit-loading-test.log`。包工具、规范生成和 ESP32-S3 Guest AOT 打包通过：`blob-read-package-test.log`、`blob-read-final-spec-test.log`、`blob-read-scenes-esp-package.log`。两个模拟器入口已构建：`blob-read-{pai,sensecap}-simulator-build.log`。

当前 READ 不保留认证块缓存；每次请求重新认证所在块，4064 B 结果后的 32 B 尾部请求会重读该块。本例 8193 B 文件的认证读取量按请求序列推导为 16385 B，尚未声称是设备总线实测流量。完整加载性能报告须计入这种开销，再评估有预算的短期块复用是否值得增加状态和驻留内存。

本阶段没有变更像素循环，没有使用 ESP 实机。UI 冷加载异步化及解码预算、音效显式预加载/完整播放通知、并发音乐与音效场景、完整平台内存汇总及最终生命周期/性能矩阵仍未全部完成，目标继续保持进行中。

本阶段最终两板固件顺序构建通过：`blob-read-sensecap-build.log`、`blob-read-pai-build.log`，两者均通过 85 个 codec 对象的分配链接审计。`blob-read-final-object-check.json` 记录关键对象相对于最终源文件的时间检查及固件摘要，源码身份与上述测试时一致。

新增并实际执行通过的一键入口：

```sh
./tools/test-pxa-resources.sh
```

脚本覆盖 native sanitizer/Core/WAMR（当前自动发现配置为 26 项）、完整 ESP 后端主机模拟、规范与包工具、重新生成并签名打包样例、产品模拟器构建、默认/低预算各 100 场景、数据读取中退出及纹理加载中退出。它验证当前已实现资源链路，不代替尚未完成的 UI/音频综合验收，也不自动执行两个 ESP 固件构建。支持 `PXA_RESOURCE_TEST_ROOT`、`PXA_RESOURCE_TEST_PROFILE`、`PXA_APP_SOURCE_ROOT`、`WAMRC` 等既有环境配置。

## 已落地：Assets 1.3 短 PCM 预加载与 Audio 0.6 句柄播放

- 从 raster 对象中提取共享不可变 `asset_object`，纹理保持原来的对象布局和直接像素视图；增加 1..16000 B、U8/16 kHz/mono PCM 驻留类型。PCM 使用现有目录、缓存和 worker，直接读入最终分配，EOF 与签名清单 SHA-256 全部验证成功后才发布。没有第二套音频文件缓存或新增线程。
- Assets LOAD/PREFETCH/STATUS 支持驻留 PCM；Guest `pxa_assets_load_sound` 返回完整代际资源句柄。两平台 STATUS 最初遗漏了 PCM，真实 Pixel Dungeon AOT 冒烟测试发现后已修复，两个 worker 测试均增加 READY 状态检查。
- Audio `play-sound` direct I/O 为固定 12 B：资源句柄、Q8 增益和保留字节。Core 检查资源类型和组件所有权；两后端检查会话与 graph 状态，最多六路声音，满时立即背压。播放保留对象引用，不复制 PCM、不读文件、不分配堆；结束时只减少消费者引用，实际缓存回收由资源 worker 执行。
- 删除 desktop 与 ESP 各自的八项短 PCM 路径缓存及同步冷加载。旧 `play_file(...pcm)` 必须迁移到异步准备加句柄播放，Ogg 保持原流式路径。音频资源、动态纹理和文件纹理使用共享预算；关闭 Guest 句柄、停止会话和缓存淘汰各自遵守独立引用。
- Pixel Dungeon 已声明 Assets 服务，使用八个固定 Guest holdings、六项待播放 ID 和最多一个 LOAD；热音效直接触发，冷音效等事件完成，替换持有不截断播放引用。事件分发纳入 Assets，取消后晚到成功仍被关闭。控制器原生测试与重新打包的签名 AOT 分别验证逻辑和实际运行链路。
- `resource-scenes` 预加载 160 B PCM，每场景触发一次，共 100 次；最终再次播放并立即关闭资源句柄和会话。新增 `exit-sound-loading` 在 PCM 最终对象已分配、真实文件读取仍在延迟中时退出。未同时播放长音乐，因此仍不能将其算作音乐/纹理并发欠载验收。

### 当前可复现证据

`tools/test-pxa-resources.sh` 现额外构建并运行真实 SDL 音频后端、跨类别预算竞争、Pixel Dungeon 控制器及签名 AOT 冒烟测试，包含 PCM 加载中退出。完整一次通过的入口日志为 `build/resource-refactor/pcm-final-checks.log`，各阶段日志在 `pcm-final-checks/logs/`：Core/WAMR 26 项 ASan/UBSan、ESP 主机适配器、规范、包工具、六个实际 AOT 运行均通过。

| 实际运行 | 共享预算内部峰值 | 外部峰值 | TEMP 外部峰值 | 退出剩余 |
| --- | ---: | ---: | ---: | ---: |
| 默认预算，100 场景与音效 | 560 B | 491564 B | 4120 B | 0 B |
| 外部总额度 300000 B，100 场景与音效 | 560 B | 298932 B | 4120 B | 0 B |
| Pixel Dungeon 预加载/音乐/焦点冒烟 | 560 B | 423732 B | 0 B | 0 B |

数值来自此次固定路径下的运行，元数据包含包路径长度；不得与使用不同包路径的阶段数据直接相减。TEMP 是总额度的子项，不能再次相加。缓存自身的逻辑预留也不能再次加到共享预算上。桌面 codec 私有内存、线程库、Guest/AOT、显示与部分 UI 分配仍不在本表内；这是共享分配器报告，不是全系统 RAM 或 ESP 实测。

低预算下 99 次可选预取被拒绝，正常加载有界回收重试后完成全部场景，声音引用未被压力淘汰。三种加载中退出均排空 worker 并回收应用预算。Pixel Dungeon 冒烟实际运行原 Ogg，检查声音资源准备、输出消费、焦点暂停保持位置与恢复消费，然后正常退出；不代表全部玩法、完整音效交互或音频欠载验收。

完整性测试额外覆盖头部为空的 PCM 路径：4097 B 跨块读取、首块后取消、同长度篡改、截断及超过 16000 B 的声明；失败不发布对象且分配归零。ESP 输出测试覆盖六声部占满、第七次拒绝、暂停/停止、句柄独立引用及预算完全占满时热播放不新增分配。SDK 检查 64 位句柄、保留字节、增益范围与截断句柄拒绝。

阶段源码身份记录在 `pcm-source-identity.json`，实际运行及原日志摘要为 `pcm-preload-measurements.json`。当前资源样例已生成 ESP32-S3 AOT（`pcm-scenes-esp-package.log`）；两个桌面模拟器入口构建通过（`pcm-preload-sim-entries.log`）。最终固件和扩展 Guest SDK 回归结果补记于本节后。

最终补验：`test_guest_sdk.sh` 完整通过，日志 `pcm-preload-sdk-final-test.log`。首次扩大回归发现 voxel 绘制命令测试仍调用已删除的同步上传函数；该函数原先在这个传输 mock 中并不验证纹理内容，现移除过时调用，保留全部绘制命令断言，文件加载与像素一致性继续由实际 AOT 集成测试承担。

两个板子的固件已顺序构建成功；发现 PCM STATUS 遗漏后补建 Sensecap，最终日志为 `pcm-preload-sensecap-final-build.log` 和 `pcm-preload-pai-build.log`。两者均通过 85 个 codec 对象及四个强预算入口的链接审计。`pcm-final-firmware-identity.json` 保存固件大小/SHA-256，以及各板 11 个相关目标文件晚于对应源码修改时间的检查；355 个阶段源码摘要复核无变化。构建成功不代表 ESP 实际音频、PSRAM 带宽或运行速度已经测量。

### 原目标继续保留

下一步仍需补齐音乐 accepted/ready/ended/error/stopped/replaced 通知、消费前完整性策略与播放实例身份；验证音乐、音效和纹理同时工作及欠载恢复。UI 冷读取/解码异步化及完整分配记账、desktop codec 私有分配、全系统同时存活峰值、完整生命周期矩阵和最终冷/热加载与帧时间对比都未完成。短 PCM 链路完成不缩减这些范围，不据此声明整个重构完成。

本次实际重跑入口日志：`blob-read-one-command-test.log`，详细日志在 `build/resource-refactor/recheck/logs/`。使用绝对包目录后固定元数据比前一组相对路径增加 87 B，低预算峰值相应为 298675 B，仍低于 300000 B；不能把路径存储差异解读成缓存增长或泄漏。两组均验证退出归零，Guest 事件缓冲峰值保持 4096 B。

## 已落地：Audio 0.7 播放实例与有界状态通知

- `play-music` direct I/O 返回完整 64 位播放实例，事件同时携带完整 Guest 会话和实例；READY、ENDED、STOPPED、REPLACED、ERROR 分开。Core 在运行时线程查找当前 provider 会话并投递，worker 只更新状态和唤醒，不进入 Guest。旧 Ogg 入口复用同一引擎，没有第二套解码器。
- 共用 `audio_playback` 保存八个实例，每项同时容纳可选 READY 和一个终态。Core 队列满时不消费后端记录，下一轮重试；实例槽耗尽立即背压。关闭 authority 清理记录，单调实例编号不重置、不回绕。原生 `sizeof` 测量为记录 24 B、邮箱 208 B、Host 事件结构 24 B，记录在 `music-events-layout.json`；这是新增固定控制存储，不是内存下降。另有 backend 函数指针、播放标记和 Guest 字段开销，不能只计邮箱便称为总增量。
- ESP 保留 32 位原子取消编号，避免引入 Xtensa 64 位原子依赖；64 位通知编号受现有 mutex 保护。所有拒绝检查在替换旧播放前完成，长度为一的命令队列通过 FreeRTOS 保证成功的 overwrite 提交，显式释放被替换命令的预算 pin。预算、实例容量和取消编号耗尽均不破坏旧播放。
- ESP 结束通知等待 ring 排空及当前输出调用返回，写失败先锁存 ERROR 再撤销在途标记，避免解码线程抢先报告 ENDED。测试覆盖这一顺序与音频淡入首采样可能为零的情况。该边界仍是 Host 输出回调完成，不是 RPC/SDL/codec/DAC 的硬件排空证明；需要精确硬件完成语义时仍须扩充设备契约。
- `resource-scenes` 同时循环 Ogg、加载纹理和播放 PCM，匹配 READY 后推进 100 个场景，最后匹配 STOPPED 后关闭。Pixel Dungeon 使用实例接口，READY 后淡入；忽略旧曲/其他会话通知，换曲同步背压时保留旧曲并在后续 tick 重试，异步 ERROR 清除失效状态。真实 AOT 冒烟测试以 READY 后才能发生的增益变化证明事件已穿过 Core/WAMR 到达 Guest，随后验证焦点暂停/恢复。

### 通知阶段验证

- `music-events-checks.log` 及 `music-events-checks/logs/`：一键入口完整通过，Core/WAMR 28 项 ASan/UBSan、ESP Host、规范、打包、真实 SDL Vorbis/Opus、Pixel Dungeon 控制器和签名 AOT，以及资源样例默认/低预算各 100 次与三个加载中退出。
- 共享状态机测试覆盖容量、READY/终态顺序、重复完成、关闭和编号耗尽。`audio_notification_test` 使用真实 Core 事件池制造背压，验证保留、恢复和旧 provider 丢弃；最初空队列断言误用 NOT_FOUND，已按 Core 契约改为 WOULD_BLOCK 后重跑通过。
- `music-events-esp-boundaries-test.log`：ESP sanitizer 主机测试通过；新增 provider/64 位实例透传、初始暂停、被拒绝的新曲、关闭清理、输出失败与 decoder EOF 边界。ESP codec 在该测试中为 mock，不冒充目标平台实际解码性能。
- 本轮默认共享预算内部峰值 560 B、外部 491607 B；300000 B 低额度下外部峰值 298975 B，99 次可选预取失败但所有场景恢复完成。Pixel Dungeon 外部峰值 423735 B。三者退出共享预算归零，细项及日志摘要见 `music-events-measurements.json`。不同路径的元数据差异不能作为优化收益；desktop codec、SDL 缓冲、Guest/AOT 等仍不全在共享预算内。

当前仍只对资源读取注入延迟，音乐没有通过同一受限存储注入层；不能据此宣布并发欠载验收完成。消费前音乐完整性、共同存储竞争/欠载与恢复、UI 异步解码及完整记账、desktop codec 私有内存、全系统同时间轴内存与最终同画质性能对比，以及完整应用生命周期矩阵仍继续执行。本阶段不缩减原目标。

扩展 Guest SDK 与应用回归完整通过（`music-events-sdk-final-test.log`），两个板配置的桌面 product/desktop 入口构建通过（`music-events-sim-entries.log`），资源样例与 Pixel Dungeon 的 ESP32-S3 AOT 重新打包通过（`music-events-esp-apps-package.log`）。Xtensa 交叉编译对象符号大小确认 ESP32-S3 的 mailbox=208 B、record=24 B，证据为 `music-events-target-layout.txt`；这是目标布局测量，仍不代表运行时整体内存。

两个板子的最终固件已顺序构建通过：`music-events-sensecap-build.log`、`music-events-pai-build.log`，两者均通过 85 个 codec 对象及四个强预算入口的链接审计。阶段身份记录为 `music-events-source-identity.json`、`music-events-firmware-identity.json`；后者逐项记录关键源码与对象摘要，检查对象不早于源码、最终二进制不早于相关对象。构建完成不代替 ESP 实际运行、设备排空或存储延迟测量。

## 进行中：PXRI 1.2 音乐块认证与有界读取器

这是消费前音乐校验的基础阶段，**生产 Ogg 解码器尚未切换到新读取器**，不能据此声明音乐完整性验收完成。

- 编译器生成 PXRI 1.2，Ogg Opus/Vorbis 与 blob 一样在路径后附每 4096 B 一个 SHA-256。PCM、PXR 和 PNG 的记录布局不增加摘要表；旧 1.0/1.1 仍能解析元数据，但旧音乐无法建立认证块映射。未知版本、缺失摘要和尾部长度均拒绝。沿用已签名索引，不新增签名体系或全曲副本。
- `pxa_asset_catalog_block_map` 一次取得借用映射，`pxa_asset_block_map_get` 按偏移 O(1) 定位，避免每次解码读取扫描所有资源。映射不复制整个摘要表；Host 必须让目录、manifest 和包存储活到最后一次流读取完成。生产后端接入时仍需落实这项生命周期责任。
- `pxa_asset_load_block` 复用现有短读、校验和取消逻辑，支持 blob 与编码音乐。Guest READ 保持只接受 blob，不允许通过新 Host 原语绕过 Guest 种类限制。
- 新 `pxa_asset_stream` 使用调用者提供的一块 4096 B 校验缓冲，无堆分配、无线程、无内部锁；块校验成功后才复制给解码器。块内短读和 seek 命中同一缓存；跨块返回短读，EOF 才返回零长度。认证/I/O/取消错误保持到流销毁，seek 不能绕过损坏块。解码器输入缓冲与这块校验缓冲不能重叠，二者仍须分别核算。
- POSIX `select_range` 复用一次安全打开的文件描述符，重新定位和初始化摘要，不逐块重开路径。路径被替换为软链接后，旧描述符仍读取原文件，新打开则拒绝软链接；原文件内容被修改时由块摘要拒绝。它保留每块的 OpenSSL hash 上下文开销，尚不能宣称完全无额外分配。

### 基础阶段证据与成本

- `stream-auth-core-test.log`：29 项 Core/WAMR ASan/UBSan 全部通过，新增真实文件 `pxa_asset_stream_test`。两种 Ogg 标识的 fixture 都由实际 Python 编译器生成索引，再由生产 catalog、POSIX SHA 和块读取器验证。覆盖多次循环、块内 cache、跨块短读、EOF/越界、加载后取消、描述符路径替换、同长度篡改、截断，以及旧索引拒绝。fixture 不是可播放音频，不冒充实际 Vorbis/Opus 解码测试。
- `stream-auth-compiler-test.log`：8 项编译器测试通过；`stream-auth-foundation-checks.log`：完整一键资源回归通过，包括 ESP Host、规范/包工具、SDL 原有解码路径、真实 Pixel Dungeon AOT、默认/低预算各 100 场景及三个加载中退出。原始日志已归档为 `stream-auth-foundation-logs/`，摘要见 `stream-auth-foundation-log-index.json`。上一通知阶段的六份实际测量日志已按原 SHA 保存到 `music-events-measurement-logs/`，其测量 JSON 已指向归档。
- 新索引的成本见 `stream-auth-index-cost.json`：资源样例 index=1132 B，其中音乐块摘要 64 B；Pixel Dungeon index=16448 B，其中六首音乐的块摘要共 15040 B。本轮共享预算外部峰值分别为默认场景 491671 B、低额度场景 299039 B、Pixel Dungeon 438775 B，退出均归零。相对上一阶段的增长正好对应索引增加，不是内存优化收益。
- `stream-auth-esp32s3-compile.log`：catalog/loader/stream 三个模块经 ESP32-S3 交叉编译器、`-Wall -Wextra -Werror` 编译通过。当前阶段尚未重新执行两个完整固件构建；上节固件证据只属于 Audio 0.7 阶段。

下一步接入 desktop Vorbis/Opus callbacks 与 ESP 编码输入，确保排队播放、换曲、关闭和应用退出期间目录仍被持有；去除音乐路径的直接未认证文件读取。随后把音乐与纹理接入共同存储限制并验证欠载恢复。UI、完整内存统计、应用生命周期矩阵与最终性能对比等原目标继续保留。

## 已接入 desktop：真实音乐解码消费认证块

- 产品 `audio_play_music` 只查已验证目录并提交块映射，移除同步 `realpath` 与路径打开。worker 用 POSIX 安全描述符和 `pxa_asset_stream` 供给实际 Vorbis/Opus callbacks，根据已认证编码选解码器，不再分别尝试两个直接文件入口。未登记/旧索引资源明确拒绝；接受后的打开、认证、格式和额度错误通过对应实例 ERROR 返回。
- 原生读取器支持解码器 read/seek/tell，换曲/停止在认证块边界检查实例取消。循环 seek 失败报告错误，不能伪装成自然结束。校验缓冲从应用 TEMP 额度分配，正常结束、错误和换曲均归还。关闭最后一个会话与产品退出先 join 解码线程，再销毁借用的目录及 manifest；没有为每个请求复制摘要表。
- 移除 Host 与 worker 的完整 music path 数组，使用目录中的借用路径。新增 `PXA MUSIC INPUT` 统计成功认证字节、块加载与块内缓存命中；它不是全部物理读取量，也不包括失败块读取成本。实时 SDL 混音仍只读取已有 PCM ring。
- 为只有音乐的应用准备目录时，不启动额外资源线程；POSIX worker 延迟到第一次 LOAD/PREFETCH/READ 接受之前创建线程，失败立即拒绝。Native 音频测试在整个播放期间验证资源线程栈报告为零；资源 worker 测试验证第一次数据读取后报告配置的 128 KiB。该数字是原生线程配置，不是 ESP RAM 或进程 RSS 的节省。仅合成音/PCM 且没有 Ogg 的应用不会因此创建目录 worker；没有声明 Assets 的音乐应用使用最小缓存控制表。
- 原来的地图加载中退出测试用 TEMP 非零猜测读取状态，音乐也使用 TEMP 后会误判。现改为观察实际 blob read 回调，仍覆盖延迟读取期间退出。PCM 与纹理退出测试及 Guest 4096 B 事件缓冲上限继续保留。

### 发现并修复的低预算启动顺序问题

最初的 300000 B 验证出现前台 LOAD 失败，另一轮在地图未完成时超时，证据保留于 `stream-auth-desktop-checks.log` 与 `stream-auth-low-budget-test.log`。样例可能先 pin 住四张纹理，再申请地图校验缓冲；加入音乐的长期 4 KiB 校验块后，等待地图的场景占住了地图需要的额度。

现在先完成进入场景必需的地图读取，再加载四张纹理；音乐/音效准备仍可与地图并行，游戏阶段持续同时运行音乐、纹理加载和音效。未扩大预算，也未放宽“所有 LOAD 失败都必须来自可选预取”的原断言。最终低预算下 100 次切换全部正确，99 个失败均为可选预取，前台没有失败，退出归零。样例仍有有界场景重试，不能把先前超时算作通过。

### Desktop 阶段证据与剩余范围

- `stream-auth-desktop-checks-final.log`：完整一键回归通过，含 29 项 Core/WAMR sanitizer、ESP Host 既有后端、规范/包工具及实际应用。之后调整了 SDL converter/循环失败状态和仅音频应用的目录条件，已重新构建四个产品测试目标，并在 `stream-auth-desktop-final-logs/` 中重新跑过真实 SDL、共享预算、Pixel Dungeon AOT、默认/低预算场景与三个加载中退出；该目录是此阶段最终产品证据。
- 新 `test_audio_backend.py` 复制音频 fixture 并运行真正的资源编译器。实际 Vorbis、Opus 各消费完整 16000 个采样，继续验证 READY/ENDED、替换背压、暂停/停止；另外验证已接受音乐在同长度篡改、缺失文件和 TEMP 耗尽时分别返回 DENIED、NOT_FOUND、RESOURCE_LIMIT。损坏输入不产生 READY，关闭后预算归零。fixture manifest 由测试提供，真实签名链路由 Pixel Dungeon 与 resource-scenes AOT 测试覆盖。
- 最终实际结果见 `stream-auth-desktop-measurements.json`：默认场景外部共享峰值 495783 B，300000 B 限额下峰值 299031 B，Pixel Dungeon 442887 B，内部峰值均为 560 B，退出均为零。音乐独立 TEMP 峰值 4112 B，与地图的 4120 B 重叠时 TEMP 峰值 8232 B；这是额外校验的明确成本。TEMP 是总预算子项，不能再次相加。
- `stream-auth-desktop-sim-entries.log`：Pai Touch / Sensecap Watcher 两个配置的 product 和 desktop 模拟器入口均构建通过。本阶段没有宣称两个 ESP 固件已包含认证音乐解码；ESP 生产输入仍待迁移。

剩余工作继续包含：ESP 解码输入与队列中目录引用的持有/释放；共同存储竞争与欠载恢复；UI 异步准备及真实解码内存；完整 native codec/线程/显示内存记账；实际应用异常与生命周期矩阵；最终同画质冷/热加载、渲染与帧时间对比；两板最终顺序固件构建及完整目标审计。桌面消费前认证完成不等于整个目标完成。

## 已接入 ESP：音乐消费前认证与目录引用

本轮完成 ESP 生产解码输入迁移。`PxaAudioOutput::RunMusic` 不再使用 `fopen/fread/fseek`；已有解码任务通过 `pxa_esp_music_input` 和共享 `pxa_asset_stream` 读取，完整认证每个 4096 B 块后才把数据交给解码器。块边界的短读不再被当作 EOS，循环定位失败、认证失败与 I/O 失败按播放实例报告错误；解码器内存不足和不支持格式分别映射为 RESOURCE_LIMIT 与 UNSUPPORTED。

- 播放请求先验证目录中的 Ogg 类型和块表，并从捕获的应用 TEMP allocator 预留固定输入工作区，整个准入不读文件。目录引用与工作区随命令转移；拒绝新命令不改变旧播放。当前解码、队列替换、过期命令、STOP 和结束/错误路径均释放各自引用，释放不在音乐锁内执行。
- `pxa_esp_assets_end` 在任何排队或正在读取的音乐输入还存在时返回 WOULD_BLOCK，保留 manifest、目录、根路径及 I/O 配置。实际退出先 reset/停止音频，再销毁资源服务；最后一个输入释放时通知持久 Host。取消发生在阻塞读取期间时，即使物理读取随后成功也不会发布该块。
- 只声明 Audio、没有声明 Assets 的 Ogg 应用也会初始化目录，使用最小缓存控制表；不会因此注册未声明的 Guest Assets 服务。没有 Ogg 的纯 PCM/合成音应用不创建该目录。ESP 仍使用已有的持久资源 worker，尚未像桌面那样延迟创建该任务。
- ESP 音乐与纹理读取现在经过同一个 `before_read` 注入入口；这只是接入共同限制的条件，**尚未完成带宽/突发停顿、调度优先级和欠载恢复的竞争验收**。

### 内存成本与验证边界

两板刚构建的 Xtensa 对象经 GDB `sizeof` 检查，输入工作区在 Sensecap 为 4304 B、Pai Touch 为 4312 B；加上 ESP 的 8 B 预算包装后分别计费 4312 / 4320 B。差异来自所用 IDF 类型布局，不能用桌面对象大小代替。这里包含 4096 B 校验块，但不包含 mbedTLS 私有分配。命令由原来的 528 B（含 512 B 路径与预算 pin）减为 16 B；设备命令队列因此少用 512 B。运行中的输入、排队输入和候选替换的输入可能同时存在，都计入 TEMP；固定 20 KiB 解码栈并没有因局部变量缩小而减少。不能把命令缩小宣称为整个音频系统内存下降。原始大小与解释见 `stream-auth-esp-layout.log`、`stream-auth-esp-pai-layout.log`、`stream-auth-esp-layout.json`。

真实 ESP 资源适配器主机测试使用 mbedTLS、真实文件、pthread RTOS stub 和真正编译器输出的 PXRI 1.2，覆盖三块读取与短读/EOF/循环、路径/种类拒绝、准入内存失败、缺失文件、同长度篡改、粘性取消、排队未打开时退出、读取阻塞时退出及下一代重新激活。关闭后应用预算与分配全部归零。Ogg fixture 仅含合法识别标记，用于证明输入认证，不是 ESP 真正 codec 的播放测试。另一个模拟 codec 测试验证输入错误不会调用解码处理或产生 READY，并继续覆盖旧/新应用预算归属、队列替换、通知背压、输出拒绝和临时内存恢复。真实 Vorbis/Opus 消费测试仍由桌面执行。

### 本轮证据

- `stream-auth-esp-host-final.log`：ESP 主机 sanitizer 回归通过，含上述阻塞读取期间退出测试。
- `stream-auth-esp-checks.log` 与归档 `stream-auth-esp-final-logs/`：完整一键回归通过，29 项 Core/WAMR 测试、ESP Host、规范、包工具、真实桌面音频和签名 AOT 场景全部通过。默认与 300000 B 低预算场景各完成 100 次切换；低预算 99 个失败仍全部为可选预取，没有前台 LOAD 失败，所有退出场景归零。六组本轮桌面观察单列于 `stream-auth-esp-desktop-regression.json`，仍不等于整个进程或真机内存。
- 两板已按 Sensecap → Pai Touch 顺序完整构建通过：`stream-auth-esp-sensecap-build.log`、`stream-auth-esp-pai-build.log`，各自通过 85 个 codec 对象与四个强预算入口的链接审计。阶段源码/对象/固件身份见 `stream-auth-esp-source-identity.json`、`stream-auth-esp-firmware-identity.json`，原始日志摘要见 `stream-auth-esp-log-index.json`。本轮没有连接 ESP 设备，不宣称固件实际播放、设备输出排空或真机延迟已经验证。

- `stream-auth-esp-scenes-package.log`、`stream-auth-esp-pixel-package.log`：resource-scenes 与 Pixel Dungeon 的 ESP32-S3 AOT/签名包重建通过，检查两个资源索引均为 PXRI 1.2；AOT、索引及 `.pxa` 摘要见 `stream-auth-esp-packages.json`。

下一阶段继续执行共同存储压力、音乐欠载计数与恢复，随后完成 UI 异步准备/解码预算、完整内存统计、真实应用异常和生命周期矩阵、voxel 同画质内存与多轮性能对比、最终 SDK/两入口/两板复验及逐项完成审计。完整目标仍未完成；本节没有缩减剩余验收范围。

## 用户修订落地：完整性只在安装时校验

本节取代前面阶段的运行时块认证方案及对应验收要求。安装/更新继续验证签名与完整文件摘要；运行时直接信任安装结果，不检测安装后文件修改。历史日志仅用于比较，不再要求恢复其中的 SHA 校验或篡改检测用例。

- PXRI 1.3 不再生成 blob/Ogg 块摘要表；旧 1.1/1.2 的表只跳过，不参与加载。资源索引、纹理、PCM、地图和音乐读取均不再创建 SHA 上下文。每次激活独享的缓存也不再为命名空间计算 manifest SHA；缓存已有清单摘要字段只作元数据键比较，不重新散列文件。
- 音乐直接读入解码器提供的缓冲，没有独立校验块或块内 memcpy；顺序读取不重复 seek。输入状态及目录引用仍随排队、替换、取消和退出正确释放。保留路径归属、格式、长度、预算、I/O 和生命周期检查。
- Blob READ 按实际请求长度分配并直接读取到结果区，删除对齐过读及 memmove。8193 B 地图的顺序读取从旧路径的 16385 B 源数据读取降至 8193 B；这是逻辑读取量，不是设备物理流量。SDK 借用结果的生命周期及 4064 B 数据上限不变。
- Assets 升为 **2.0**。旧 1.x SDK 解析器会拒绝跨 4096 B 边界的结果，不能以 minor 升级掩盖语义变化；旧 Guest 应重新编译并打包。PXRI 版本与服务版本独立，Core 信封仍为 v1。首次应用集成发现遗留 Core/SDK 边界检查导致地图等待超时，修复后重新完成全部回归；失败日志 `install-trust-checks-r2.log` 保留，不计作通过。
- ESP 移除启动/枚举时重验安装包的配置和分支。POSIX `load_current` 读取已提交 manifest；模拟器 `load_directory` 读取已准备目录，不再通过完整 `verify_source` 启动。接收/安装原始包的入口继续校验。打包、安装和恢复事务使用的 SHA 没有删除。

### 实测内存与索引

以下是同一资源预算口径的桌面结果，不是进程 RSS、完整 Host 内存或 ESP 实测。本阶段还包含共同存储调度入口及桌面预缓冲计数基础，所以总差值不当作单独 SHA CPU 基准。

| 场景 | 旧外部预算峰值 | 当前外部预算峰值 | 当前 TEMP 峰值 |
| --- | ---: | ---: | ---: |
| 默认 100 场景 | 495783 B | 491791 B | 4088 B |
| 300000 B 限额的 100 场景 | 299031 B | 295039 B | 4088 B |
| Pixel Dungeon 音频 | 442887 B | 424015 B | 0 B |

默认/低预算旧 TEMP 峰值均为 8232 B，Pixel Dungeon 旧值为 4112 B；TEMP 是总预算子项，不重复相加。各场景退出的应用预算均归零。原始日志归档于 `install-trust-final-logs/`，数据及旧版对照见 `install-trust-measurements.json`。

资源样例索引从 1132 B 缩至 972 B；Pixel Dungeon 模拟器索引从 16448 B 缩至 1408 B，ESP 包索引从 8512 B 缩至 1408 B。两目标的原音乐资源不同，因此各自与自己的旧包比较。当前实际 Xtensa 对象中，两板音乐输入状态均为 176 B，加 8 B 预算包装计费 184 B；旧 Sensecap 为 4304 B、旧 Pai 为 4312 B。没有以桌面 sizeof 推测设备布局，也没有声称固定解码栈、PCM ring 或 codec 工作区同时缩小。

### 已通过的运行验证

- `install-trust-checks-r3.log`：一键资源回归通过，含 31 项 Core/WAMR ASan/UBSan、ESP Host 回归、规范/打包检查、真实 Vorbis/Opus 解码、签名 Pixel Dungeon AOT、默认及低预算各 100 场景，以及地图/纹理/音效加载中退出。低预算 99 个失败全部属于可选预取，无前台 LOAD 失败。安装器的内容/签名拒绝测试继续通过。
- `install-trust-sdk.log`：Guest SDK 与应用测试通过。resource-scenes、Pixel Dungeon、voxel-craft 的 ESP32-S3 AOT 和签名包均已重建，manifest 均要求 Assets 2.0，索引均为 PXRI 1.3；见 `install-trust-packages.json`。
- `install-trust-shared-storage.log`：实际 100 场景及音乐共同经过 2097152 B/s、每读额外 500 μs 的 POSIX 存储调度器，同时保留纹理原有 1000 μs 注入。资源读取 26237069 B、音乐读取 35816 B，实际音乐最大排队 2931 μs，READY 最大 30555 μs，欠载为零，退出预算归零。此轮仅验证供给充足场景，不代表突发停顿、供给不足恢复、解码成本、ESP 调度或真机存储已完成。

- `install-trust-sensecap-abi2-build.log`、`install-trust-pai-build.log`：Sensecap → Pai Touch 顺序固件构建通过，两板各通过 85 个 codec 对象及四个强预算入口审计；`install-trust-sim-targets-final.log`：两配置的 product/desktop 模拟器入口均构建通过。阶段源码、固件对象、日志身份分别见 `install-trust-source-identity.json`、`install-trust-firmware-identity.json`、`install-trust-log-index.json`。16 个运行时资源对象的符号检查未发现 SHA/摘要上下文依赖，见 `install-trust-no-runtime-hash-symbols.json`；安装器对象保留这些依赖。
- `install-trust-voxel-capture.log`、`install-trust-voxel-comparison.json`：voxel-craft 重新打包为 Assets 2.0 并在真实 WAMR/product 循环中运行，48 张纹理、调色板及完整菜单与先前快照逐字节一致，退出共享预算归零。这不是完整游戏过程或帧时间基准。没有连接 ESP 设备，本轮不宣称真机播放、存储速度或内存实测。

完整重构目标仍保留：共享存储突发停顿与音频恢复、ESP 预缓冲、UI 异步准备/真实解码预算、完整内存统计、应用异常与生命周期矩阵、voxel 同画质及多轮 p50/p95 性能对比。运行时 SHA 与安装后修改检测不再属于剩余工作。

## 已落地：共享存储停顿恢复与 ESP 预缓冲

桌面和 ESP 音频现在共用 `pxa_audio_buffer`。已有 8192 样本、16 KiB 的单声道 PCM ring 容量保持不变；首次输出及欠载后恢复以 4096 样本为阈值，EOF 可以放行短尾段。一次取样不足只增加一次欠载，重新缓冲期间统计缺失样本，恢复不重复发送 READY。暂停期间不增加欠载，正常 EOF 不作为欠载；ESP 先发布 EOF 再等 ring 与在途输出排空，短曲不再依赖填满阈值。

- ESP `QueueMusic` 只在首次满足准备条件时唤醒 READY 通知；每批补充 PCM 不再重复唤醒 Host。输出任务只消费就绪 ring，解码及文件操作仍在原 decoder task，沿用已有工作区和生命周期约束。
- 两端统计消费/缺失样本、高低水位、欠载/恢复次数及接受到 READY 的时间。低水位采样排除初始预缓冲和自然 EOF，以 `low_water_valid` 区分未采样；计数不会把暂停误判为供给不足。
- 桌面解码调用的墙钟耗时减去输入回调时间，从而不把 3 秒文件停顿记成 3 秒解码计算；调度器抢占仍包括在墙钟时间里。ESP 分别记录文件读取与 `esp_audio_simple_dec_process` 耗时、最大耗时、读取字节与块大小。`GetMusicStats` 是短锁保护的只读快照，曲目退出时输出设备累计统计；实时输出路径不输出统计日志，也不新增 Guest 事件。

### 实际 AOT 与存储竞争结果

新增测试通过真正的 resource-scenes AOT、Core/WAMR、纹理 worker、Vorbis 解码线程及 SDL 输出运行。存储限制对音乐与资源的 payload read 共同生效：2097152 B/s、每次额外 500 μs，最大请求 4096 B；纹理原有 1000 μs 注入继续保留。文件系统元数据、缓存及物理介质延迟不是精确设备模型。测试直接在生产共享读取调度器注入停顿，主循环仅作观察与控制，不替代 decoder 或 mixer。

| 场景 | 欠载 / 恢复 | ring 高 / 低水位 | 主循环观测 |
| --- | --- | --- | --- |
| 带宽与单次延迟，100 场景 | 0 / 0 | 6656 / 5760 样本 | 无前台加载失败 |
| 3 秒共同停顿，100 场景 | 1 / 1 | 6656 / 0 样本 | 自停顿开始约 3.005240 秒观测到恢复；期间泵回调 728 次，最大间隔 9664 μs |
| 300000 B 限额加 3 秒停顿，100 场景 | 1 / 1 | 6656 / 0 样本 | 自停顿开始约 3.001571 秒观测到恢复；729 次泵回调，最大间隔 10877 μs |
| 两条读取路径都阻塞在 30 秒停顿中退出 | 0 / 0 | 6528 / 5760 样本 | 发出退出后 37657 μs 完成回收，无需等待停顿结束 |

恢复时间含整个 3 秒注入和主循环观察延迟，不是 ESP 恢复速度。低预算仍只有 99 次可选预取失败，没有前台 LOAD 失败，外部共享预算峰值 295039 B；默认竞争测试峰值 491791 B，各次退出均归零。固定 ring 的高水位从未超过容量，没有扩大缓存通过恢复测试。

`storage-recovery-checks-final.log` 和归档 `storage-recovery-final-logs/` 包含最终完整一键回归：31 项 Core/WAMR sanitizer、ESP 主机测试、规范和包工具、真实 Vorbis/Opus、Pixel Dungeon AOT 的暂停/恢复、共五组各 100 次场景切换，以及四种加载/存储停顿中退出。`tools/test-pxa-resources.sh` 已加入 steady、stall、stall-low-budget、exit 四项共享存储检查，并固定默认存储注入参数，避免继承 shell 设置改变基准。十种应用观察与原始日志摘要见 `storage-recovery-final-measurements.json`。

ESP 输出主机测试使用生产 `PxaAudioOutput`、真实公共 buffer 状态机、模拟 codec/device 与预算器，验证少量 PCM 不提前输出、短曲 EOF、欠载只记一次、补足恢复、READY 不重复、暂停不增加缺失样本、输出失败优先于 EOF、替换/退出及旧应用额度回收。此测试不冒充 ESP 真正 codec 在真机的播放验证。

### 固定开销与构建证据

本轮增加了小型固定统计状态。两板实际 Xtensa 对象中的 `MusicStats` 为 120 B，`PxaAudioOutput` 为 576 B；之前为 440 B，总固定对象增加 136 B，属于板级对象，不在应用资源预算中。旧头文件按上一阶段保存的 SHA 精确复原，用相同 Sensecap 编译参数生成独立布局对象；相关项目依赖类型确认未变。方法、命令和原始 GDB 结果见 `storage-recovery-layout.json` 与 `storage-recovery-old-layout-command.json`。20 KiB decoder task 栈和 16 KiB PCM ring 的配置容量均未增加；实际 ESP 栈高水位尚无设备测量。

- `storage-recovery-sensecap-final-build.log`、`storage-recovery-pai-build.log`：两板按顺序构建通过，均通过 85 个 codec 对象和四个强预算入口的链接审计。
- `storage-recovery-sim-entries-final.log`：Pai Touch / Sensecap Watcher 的 product 与 desktop 两入口均构建通过。最终阶段源码、对象及日志身份分别保存在 `storage-recovery-source-identity.json`、`storage-recovery-firmware-identity.json`、`storage-recovery-log-index.json`。

完整目标仍未完成。下一步保留 ESP 共享存储调度的明确仲裁与竞争验证，随后完成 UI 冷加载异步准备及实际解码预算、完整内存核算、应用异常/生命周期矩阵、voxel 游戏过程及同画质多轮渲染/帧时间比较，最后执行逐项完成审计。运行时 SHA 不会重新加入。


## 已落地：ESP 音乐与资源读取仲裁

ESP 资源 worker 与已有音乐 decoder 的 payload read 共用一个有界读取入口：一次最多 4096 B，一个正在读取的请求，每条通道至多一个等待者。当前读取结束后，等待中的音乐先于下一块纹理读取；原资源队列仍负责前台加载与预取的优先级。等待复用原任务通知，取消由短周期检查或退出唤醒处理，没有新增 worker、动态等待队列、逐块分配或长时间持锁。额外的同通道等待请求立即返回 WOULD_BLOCK，不覆盖已有等待者。已进入 OS 的真实 read 不可抢占；这里没有声称取消可以中断设备驱动内部阻塞。

- 注入延迟回调改为在仲裁之后执行，并接收取消检查，避免测试中的长停顿延长退出。实际文件读取、延迟注入和音频取消回调均在资源短锁外执行。通知在短锁内发送，保证等待者注销/任务退出与通知目标的生命周期一致。
- 新增每条通道的读取次数、字节数、等待/服务耗时及最大值、最大读取请求、取消和错误计数。开始新应用时清零，退出后保留最终快照；统计日志只在服务回收时输出，没有逐块 Guest 事件或日志。计时是主机/设备墙钟，含调度影响；read 字节数不是物理介质传输量。
- 生产 ESP 后端配合真实文件和 pthread/FreeRTOS 桩，验证阻塞纹理后音乐先行、额外等待者背压、音乐在纹理仍阻塞时独立取消、共同停顿中退出、读取错误后让出占用，以及四次重新激活后预算归零。音乐文件在此仅是索引/字节流 fixture，不冒充真实音频解码；真实 Vorbis/Opus 解码由既有桌面音频和 AOT 集成测试覆盖。
- `storage-arbitration-host-final.log` 的 ASan/UBSan Host 回归已通过。首次 Host 运行发现 lazy-storage 测试缺少新增统计接口的桩，补齐后通过；最初日志保留为 `storage-arbitration-host.log`，不计作完整通过。读取路径还删除了移除 SHA 后遗留的两次相邻取消检查之一，保留一次完成前取消判断。

最终验证与开销：

- `storage-arbitration-checks-final.log` 和 `storage-arbitration-final-logs/` 的 25 份日志覆盖最终代码：31 项 Core/WAMR sanitizer、ESP Host、包/规范、真实 Vorbis/Opus、Pixel Dungeon 音频与焦点切换、五组各 100 场景和四类加载/停顿中退出均通过。3 秒停顿在默认与低预算中各产生一次欠载、一次恢复；低预算仍只有 99 次可选预取失败，外部预算峰值 295039 B，没有前台 LOAD 失败。30 秒停顿中的退出耗时为 40143 μs，各应用退出预算归零。十组观察见 `storage-arbitration-measurements.json`，属于主机数据，不是 ESP 速度。
- `storage-arbitration-sensecap-final-build.log`、`storage-arbitration-pai-build.log`：Sensecap → Pai Touch 顺序构建通过，各通过 85 个 codec 对象和四个强预算入口的链接审计。`storage-arbitration-sim-entries.log` 记录两配置的 product/desktop 模拟器入口构建通过。
- 两板实际 Xtensa 布局均为：读取输入 56 B、音乐输入 176 B，与上一阶段相同；新增固定统计状态 136 B、等待任务指针 8 B，共增加 144 B，已计入 ESP metadata 报告。没有新增线程、任务栈或 payload buffer。布局原始记录及对象摘要见 `storage-arbitration-layout.json`、`storage-arbitration-{sensecap,pai}-layout.log`。
- `storage-arbitration-no-runtime-hash-symbols.json` 复核 16 个运行时资源对象，没有 SHA/摘要上下文依赖。安装验证不变。最终源码、固件对象及日志身份分别见 `storage-arbitration-source-identity.json`、`storage-arbitration-firmware-identity.json`、`storage-arbitration-log-index.json`。

完整目标仍有 UI 异步准备与解码预算、完整内存核算、真实应用异常/生命周期矩阵、voxel 游戏过程及同画质多轮性能比较等未完成项。运行时 SHA 和安装后修改检测不属于剩余要求。


## 已实现：离线 UI 图片、异步加载与节点句柄绑定

新增 Assets **2.1**、UI **0.5**。Core、两端能力表、SDK 和包工具的规范元数据同步升版；PXRI 保持 1.3 的现有记录布局。PXR1 kind=4 表示 UI IMAGE，encoding=2 为 little-endian RGB565，encoding=7 为 BGRA8888 直通 alpha。`resources.json` 的 `kind: image` 将 PNG 源图离线转换为此格式，默认 BGRA8888；显式 RGB565 必须为不透明源图。最大声明尺寸 4096×4096，但实际加载仍须通过共享预算与缓存额度，不能据此宣称所有最大尺寸均可驻留。PNG 可保留索引查询能力，未编译 PNG 的 IMAGE LOAD 返回不支持。

- 同一 POSIX/ESP 资源 worker 直接分块读取到最终不可变对象；没有在 Guest、Host 压缩缓存和 LVGL 解码像素之间复制整张图。新类型在两端按 IMAGE 计入共享预算，重复加载合并为同一对象。已有 INDEX8/调色板和 PCM 继续走原路径。
- Guest 使用 `pxa_assets_load_image`，在正常 LOAD 完成后调用 `pxa_ui_set_image(&transaction, node, handle)`。UI 的 IMAGE_HANDLE 属性为完整 u64；Core 将事务的实际组件传给 Host 查找回调，避免依赖前台组件进行授权。Host 在提交准备阶段取得引用，失败/取消释放准备引用，成功后节点持有引用；Guest 关闭 handle 不会移除当前图片。
- LVGL 适配器为准备好的像素建立描述符和借用视图，专用入口直接返回视图，不分配普通 binary decoder 的逐次打开状态，也不做像素缓存或隐式转换。准备阶段校验 stride/对齐；当前软件 UI 使用直通 alpha，预乘转换请求不偷偷分配副本。每适配器首次使用时创建一个固定 LVGL 注册对象，deinit 时注销。此对象和节点/命令/描述符元数据必须在完整内存报告中另计；不能把 IMAGE 像素预算称为整个 LVGL 内存上限。

### 本阶段覆盖范围

- 编译器测试验证 BGRA 顺序、部分/零 alpha、RGB565 字节序、透明图拒绝 RGB565；Core loader 测试验证大于纹理 256 边长的图片、短读、取消、分配失败回滚、截断和过大尺寸拒绝。Core/SDK 真实请求路径验证 IMAGE 完成解析、不同组件/类型拒绝、关闭与 stale handle。
- POSIX/ESP 的实际文件 worker fixture 新增 320×16 BGRA 和 RGB565 图片。阻塞 cold read 时请求返回且未发布半成品；两个请求取得同一对象，逐像素检查数据，ESP 进一步核对 IMAGE 分类的实际计费，退出归零。原 20 张纹理、100 次场景切换继续执行。
- 真正 LVGL 的 ASan/UBSan 测试验证 RGB565 四色输出、BGRA 借用原始像素、没有额外解码像素/解码状态、成功提交后 Guest 引用释放、事务取消、无效/跨组件拒绝保留旧图、重复绘制不增加 PXA adapter 分配与句柄查找次数、替换和退出释放像素。此处不是带新 UIImage 的签名 AOT 应用验收；新节点路径的实际应用迁移和更完整截图/生命周期矩阵仍需完成。
- `tools/test-pxa-resources.sh` 新增 LVGL 配置、构建和图片测试，默认使用模拟器固定的 LVGL 源码。首次包/SDK 联调发现 Core 服务清单及 SDK LOAD 结果解析器未同步 IMAGE，补齐后通过；首次 ESP 构建发现其 LVGL 私有头文件已移动到 src/image，加入对应版本包含路径后通过编译。初次失败记录保留，不计作最终通过。

最终验证与开销：

- `ui-image-final-checks.log` 和 `ui-image-owner-final-logs/` 的 28 份日志覆盖实际组件归属修正后的最终代码：31 项 Core/WAMR sanitizer、实际 LVGL 图片测试、ESP Host、包工具与规范、真实 Vorbis/Opus，以及既有十组 AOT 工作负载均通过。各应用退出的共享预算归零。新 IMAGE_HANDLE 路径目前由原生 Core/LVGL 与两端文件 worker 测试覆盖，不能将既有 AOT 工作负载算作新 UI 路径的应用迁移验收。
- `ui-image-measurements.json` 记录本轮主机观察：默认与低预算资源场景的外部预算峰值仍分别为 491791 B、295039 B；正常存储竞争没有欠载，3 秒停顿在两种预算中均欠载一次、恢复一次；30 秒停顿中的退出耗时 50882 μs。以上是既有资源/音频路径的回归数据，不是新 UI 图片的完整内存测量或 ESP 实测。
- 两板实际 Xtensa 对象布局相同，原始记录见 `ui-image-final-layout.json`：资源对象 24 B、view 16 B，均未增加；UI workspace 实测 616 B；节点由 120 B 增至 128 B，命令由 48 B 增至 52 B。每个准备好的图片绑定另有 64 B 描述符/借用视图包装；首次使用的 LVGL decoder 为 28 B，加链表前后指针为 36 B，尚不含底层堆管理开销。像素按 IMAGE 预算计费，上述 UI/LVGL 元数据分别列出，不能据此声称已覆盖全部 UI 内存。
- `ui-image-sensecap-final-build.log`、`ui-image-pai-final-build.log`：Sensecap → Pai Touch 顺序构建通过，各通过 85 个 codec 对象和四个强预算入口的审计。`ui-image-final-sim-entries.log` 记录两配置的 product/desktop 入口构建通过。源码、固件对象及日志身份见 `ui-image-source-identity.json`、`ui-image-firmware-identity.json`、`ui-image-log-index.json`。
- `ui-image-no-runtime-hash-symbols.json` 再次核对 16 个运行时资源对象，未发现 SHA/摘要上下文依赖；安装器继续保留校验。缓存只比较清单已有的内容身份，不重新计算文件摘要，也不检测安装后修改。

旧 ASSET 路径和 Canvas 图片还没有迁移，仍保留同步冷加载；必须继续迁移真实应用并移除该路径。准备阶段拒绝的测试不替代完整 UI 事务原子性验收，后续还需覆盖提交后期的分配失败及 alpha-plane 更新失败回滚，再完成完整内存核算、应用异常/生命周期矩阵、voxel 游戏过程及同画质多轮性能比较。运行时 SHA 没有恢复。

## 已实现：Canvas 准备好的图片引用与失败回滚

UI **0.6** 增加 `IMAGE_HANDLE` Canvas 指令（11），固定 26 B payload，沿用完整 u64 Assets IMAGE 句柄。SDK `pxa_canvas_image_handle` 只构建指令，不建立额外缓存或事件调度器。Core 传递实际提交组件，适配器在提交准备阶段验证归属、类型和句柄代际并持有像素；同帧重复句柄只创建一个包装和一份持有引用。绘制按指令顺序访问准备好的视图，即使某条指令被裁剪，也正确推进视图索引；没有逐像素或逐 draw 的句柄查找、文件读取、解码或新图片包装分配。

每次新提交重新验证句柄，不因旧帧仍持有对象而让已关闭句柄重新有效。Guest 在 PRESENT 成功后可以关闭句柄，旧画面继续显示；连续提交使用同一图片时需保持句柄打开。准备失败释放本次已经取得的全部引用，成功替换或删除节点释放上一帧。当前提交准备仍会分配有界的视图数组及每种唯一图片的包装，后续应结合真实应用测量复用成本；这里没有宣称每次 PRESENT 零分配。

同时修复既有 Canvas 的后期失败问题：原实现先释放旧字节/图片，再尝试 alpha 输出，失败返回会与 Core 保留新帧所有权的约定冲突。现在在执行锁内预览完整新帧，alpha 合成成功后才交接所有权。失败恢复旧 Canvas 状态及复用的 bitmap 描述符，并释放准备引用；不调用新帧的释放回调。单 overlay 先完成可能失败的 LVGL snapshot，再更新 plane，同尺寸更新复用旧 plane；多个 overlay 在独立的临时 plane 完成全部快照后才交换，失败保留旧像素和 revision。多 overlay 临时 plane 每像素额外 3 B（RGB565 加 alpha），以及 snapshot 和新旧 frame 元数据的重叠，必须计入完整峰值报告。普通 UI 树事务的后期失败回滚仍未因此完成。

本阶段验证范围：

- SDK 测试检查固定报文长度、高 32 位不丢失、无效参数和小缓冲；Core 检查指令长度、feature、fit 和零句柄。第一次 SDK 编译暴露公共 writer 没有 `put_u64`，改用两个 little-endian u32 后通过。UI 服务/规范/注册表统一为 0.6。
- 实际 LVGL sanitizer 测试通过 Core 提交图片 Canvas，验证同帧去重、裁剪后的顺序访问、四色像素输出、替换、Guest 关闭后继续绘制及新提交拒绝旧句柄。部分准备失败、跨组件、视图数组/包装分配失败均保留旧帧并回收引用；单 overlay 尺寸变化时和多 overlay 临时 plane 的分配失败均保留原像素指针、内容及 revision。实际 BGRA 图片合成与 LVGL 标准内存图片路径逐像素一致，并检查输出不是透明空白。
- `canvas-image-checks.log` 的完整一键回归通过，31 项 Core/WAMR、ESP Host、规范/包工具、实际 LVGL、Vorbis/Opus 与既有十组 AOT 场景均通过；`canvas-image-sdk-r2.log` 为 SDK/应用回归。追加的非空 BGRA 输出断言由最终独立 LVGL 回归覆盖。既有资源 AOT 回归不替代新 Canvas 句柄路径的 AOT 应用迁移验收。
- 为下一阶段迁移保存了 plane-shooter 的源码、25 张 PNG 清单和签名 simulator AOT 包，位于 `canvas-image-plane-baseline/`。新增 `pxsys_ui_images_test` 可捕获真实 UI AOT 的启动画面、Guest/Core UI 和共享预算计数；这只是静态启动场景，不是 gameplay/FPS 基准，LVGL 堆与解码分配仍须另行计量。

最终构建与内存证据：

- `canvas-image-sensecap-idf55-build.log`、`canvas-image-pai-gcc16-build.log`：两板顺序构建通过，各通过 85 个 codec 对象和四个强预算入口审计。Sensecap 使用原 IDF 5.5.4 / GCC 14，Pai Touch 使用当前 main SDK / GCC 16 的隔离目录 `build/firmware/canvas-image-pai`。首次误用 main SDK 与 Sensecap 原 GCC 14 缓存冲突，隔离的 Sensecap/main 尝试已停止，这两次都不计作通过。不能把编译器差异当作代码优化收益。两配置 product/desktop 模拟器入口最终构建见 `canvas-image-final-sim-entries.log`。
- `canvas-image-layout.json` 保存实际 Xtensa 对象、编译命令及 SDK revision，两板布局相同：UI workspace 616 B、节点 128 B、命令 52 B、图片包装 64 B，均与前一阶段相同；Canvas 状态由 40 B 增至 48 B，Core Canvas view 由 92 B 增至 96 B。每条图片指令额外 16 B 视图条目，同帧每种唯一图片再用 64 B 包装；像素仍共享原对象。alpha 状态为 40 B，包含在上述 UI workspace 中，事务预览在栈上保存旧状态。旧 Pai 对象已与前一阶段保存的 SHA 一致性比对后读取布局，见 `canvas-image-layout-baseline.json`；这些大小不包含底层堆管理开销。
- `canvas-image-final-logs/` 归档 28 份一键回归日志；`canvas-image-final-lvgl-ctest.log` 是追加非空 BGRA 输出断言后的最终 LVGL sanitizer 回归。十组 AOT 观察见 `canvas-image-measurements.json`：默认/低预算外部峰值仍为 491791 B / 295039 B，正常竞争无欠载，3 秒停顿在两种预算中均欠载一次并恢复一次，30 秒停顿中退出耗时 38355 μs，各次退出预算归零。这是主机回归数据，不是新 UI 路径性能或 ESP 实测。
- `canvas-image-plane-baseline-capture.log` 记录原 plane-shooter 签名 AOT 的真实启动画面捕获，像素与带作用域的内存数据在 `canvas-image-plane-baseline/capture/`。Guest 线性内存高水位 139264 B、AOT 保留缓冲 150996 B、Core UI 当前 6592 B；IMAGE 分类的 9156 B 主要是 PNG 压缩数据，未覆盖 LVGL 解码像素和原生堆，不能当作整个旧 UI 图片占用。采样发生在分配截图缓冲之前，退出共享预算归零；没有完成应用迁移或 gameplay 性能验收。
- `canvas-image-no-runtime-hash-symbols.json` 检查 16 个运行时资源对象，无 SHA/摘要上下文依赖。最终源码、固件对象和日志身份见 `canvas-image-source-identity.json`、`canvas-image-firmware-identity.json`、`canvas-image-log-index.json`。

旧 ASSET/路径 Canvas 调用仍待真实应用迁移和删除，优先利用已保存的 plane-shooter 基线推进实际 AOT 迁移与同画面比较。普通 UI 树事务的后期失败回滚、Canvas 提交元数据复用、完整内存核算与准入、应用异常/生命周期矩阵、voxel 游戏过程及同画质多轮性能验收继续保留。运行时 SHA 未恢复。

## plane-shooter 实际图片迁移与生命周期验证

飞行游戏已经使用 Assets 2.1 / UI 0.6 的异步 IMAGE 与 Canvas IMAGE_HANDLE。21 张实际使用的 PNG 原稿移到 `resources/`，构建时无损生成 BGRA8888 PXR；源图与保存的迁移前基线逐文件相同。绘制代码使用枚举 ID 和句柄，不再传图片路径。启动器图标仍使用包图标入口。

`images.c` 使用应用现有的事件分发，仅允许一个在途 LOAD。页面声明必要图片集合：首页五张，战斗准备所选飞机、首领及敌人/拾取物/面板。切换时关闭不需要的 Guest 句柄；缺图时先呈现不含图片的加载画面，解除上一帧引用，再申请新图片。失败释放部分加载结果并提供显式重试，不忙等；取消与成功竞态正确关闭不再需要的晚到句柄。后台暂停加载和画面更新，前台恢复后继续，退出统一关闭资源。

### 实际签名 AOT 与对比

- 沿用 `canvas-image-plane-baseline/` 保存的独立旧包，未用新源码重新生成旧基线。`tools/compare-plane-images.py` 通过真实 Core UI 事件驱动首页→机库→商店→简报→首页→战斗。就绪日志只用于判断截图时机，结果以原始 BGRA 像素比较为准。
- `plane-images-final-comparison/report.json` 保存完整包身份、运行程序、命令、环境、日志和原始截图。首页、后台加载恢复后的首页及默认/低预算各五个菜单均与旧包逐字节一致。三组战斗运行都验证运行中画面变化、后台画面不变、恢复后再次变化；共 15 项相等比较和 6 项变化比较。没有把不同时间的战斗画面声称为新旧逐像素一致，也没有把该流程称为帧时间基准。
- 180000 B 共享外部预算、每次资源读取额外 2000 μs 的场景完成上述切换与战斗入口。对比脚本使用绝对包路径，预算峰值 **179138 B**；默认流程为 **250754 B**。此前手动相对路径运行分别是 179063 / 250679 B，差值来自实际路径元数据长度，不能混成同一口径。低预算由缓存淘汰完成切换，未扩大上限。
- 每次读取额外 20000 μs 时，加载中后台→恢复和加载中退出通过；新路径所有运行的旧 PNG/path resolver 调用均为零。退出预算均归零。未加入安装后文件修改或篡改检测用例。

### 内存结果与限制

首页的 Guest 线性内存当前/峰值均保持 139264 B；模拟器 AOT 从 150996 B 增至 160440 B，新增场景控制及错误处理并非免费。Core UI 当前字节从 6592 B 降至 4800 B；WAMR 原生当前/峰值由 22452/28127 B 变为 22820/30923 B。

旧首页共享预算的 IMAGE 为 9156 B，主要记录压缩 PNG；新首页 IMAGE 为 101904 B，记录准备好的像素及分配包装。旧 LVGL 解码像素、解码器私有分配和堆没有纳入旧数值；新 worker 元数据与 131072 B native 栈也不能忽略。因此本阶段**没有得出总内存下降结论**。PXR 展开像素使安装后的资源文件增大，存储 I/O 与常驻像素成本必须分开报告。完整 native 同时间轴峰值仍待完成。

### 回归及交付

- `plane-images-checks.log`：完整一键回归通过，归档 `plane-images-final-logs/` 共 36 份日志；包括 31 项 Core/WAMR、ESP Host、规范/打包、真实 LVGL、原音频/场景回归，以及新增 5 组飞行游戏 AOT。`plane-images-measurements.json` 的 15 组 AOT 运行全部退出预算归零。控制器 ASan/UBSan 覆盖有界请求、完整 64 位身份、场景释放、取消竞态、后台、失败、显式重试及退出。
- `plane-images-sdk.log`：Guest SDK 与应用测试通过。`plane-images-esp-package.log`：ESP32-S3 AOT 和签名包构建成功，主 AOT 为 270216 B。没有实机，不能据此声称 ESP 运行验证完成。
- 两板前阶段固件记录中的所有相关源码与目标文件仍逐项一致，本阶段没有修改固件代码，证据为 `plane-images-firmware-reuse.json`，不冒充新一轮固件构建。两配置 product/desktop 模拟器重新构建的日志为 `plane-images-sim-pai.log`、`plane-images-sim-sensecap.log`。
- `plane-images-no-runtime-hash-symbols.json` 再次检查 16 个运行时资源对象，没有 SHA/摘要上下文依赖。安装验证仍保留。源码与日志身份为 `plane-images-source-identity.json`、`plane-images-log-index.json`。早期 walkthrough 测试把主 surface 误写成 0 的失败日志保留；改成正式 `PXA_UI_PRIMARY_SURFACE` 后重跑，失败运行不计为通过。
- 应用 `RESOURCES.md` 说明实际加载顺序、后台/退出处理、重试以及测量边界；项目 `tools/test-pxa-resources.sh` 已加入该应用的控制器、打包及运行验证。

完整目标仍未完成：其他旧 UI 图片路径的迁移/删除、普通 UI 树事务后期失败回滚、Canvas 提交元数据复用、完整内存核算与准入、其余异常/生命周期矩阵、voxel 游戏过程及同画质多轮帧时间验收继续执行。运行时 SHA 和安装后修改检测继续排除在目标之外。

## Canvas 图片元数据复用与提交性能

本阶段完成 Canvas 准备阶段的图片绑定数组和包装复用。每次提交仍通过 Core 重新获取并验证唯一句柄，不以旧帧缓存绕过关闭、代际或组件检查。

- 图片句柄序列不变时，复用整组绑定；新坐标、透明度和裁剪仍读取新帧指令。序列变化时使用备用数组，并仅在提交成功后转移之前已持有的描述符。
- 不再使用的包装先释放像素引用和 LVGL 解码缓存，再放入空包装池。容量由两帧重叠的高水位决定，不随帧数累积；无图片帧和节点销毁会释放该池及绑定数组。没有新增图片副本、线程或 Guest 内存。
- 首次借用的描述符与同帧重复项分别标记，提交时不会为每个重复项再次扫描整个旧数组。失败只释放新取得的资源、归还从池中取出的包装；旧绑定、旧画面和 alpha plane 的已提交内容保持不变。
- 包装的活跃 owner 与空闲链表指针共用一个 union，两板实际包装大小仍为 64 B。Canvas 状态从 48 B 增至 64 B；每个有图片的 Canvas 另按实际容量保留必要的绑定数组和空包装，不能将元数据复用当作没有内存代价。

### 分配与内存窗口

真实 Core + LVGL 适配器测试分别覆盖交替两种图片/改变指令数量，以及同图重复 80 次并移动位置。两个场景在预热后各执行 20000 次提交；旧、新实现使用相同 Core、LVGL 库、输入和像素 oracle，旧适配器源码与上一阶段保存的身份一致。

| 受测工作集 | 旧分配次数 | 当前分配次数 | 旧稳态 / 窗口峰值 | 当前稳态 / 窗口峰值 |
| --- | ---: | ---: | ---: | ---: |
| 交替图片及指令数量 | 50000 | 0 | 4112 / 4376 B | 4240 / 4240 B |
| 同图 80 次、交替移动 | 40000 | 0 | 13568 / 15584 B | 13768 / 13768 B |

这是测试工作集的 Core/适配器分配器字节，包含其固定节点等对象，排除 LVGL 私有堆、原生栈、测试采样数组、系统堆管理开销和完整应用的其他分配。稳态分别增加 128 / 200 B，以消除提交阶段反复分配；提交窗口峰值分别减少 136 / 1816 B。两场景的新旧唯一句柄获取数分别保持 30000 / 20000，未通过省略验证取得结果。

### Release 提交时间

`tools/compare-canvas-metadata.py` 从独立保存的适配器源码构建旧版，与当前版交替运行 5 轮，固定 CPU 8、`-O3 -g -UNDEBUG`、不启用 sanitizer。每个场景先持续预热 200 ms，采集 20000 个样本并丢弃前 2000 个；比较每轮分位数的中位数。六个工作负载快照的 hash 在新旧版本间一致。

| 工作集 | 旧 p50 / p95 | 当前 p50 / p95 |
| --- | ---: | ---: |
| 交替图片及指令数量 | 3.283 / 3.841 μs | 2.374 / 2.863 μs |
| 同图 80 次、交替移动 | 9.499 / 10.616 μs | 5.098 / 5.797 μs |

该指标仅包含 Core Canvas 提交、准备元数据及失效标记，不含实际光栅化、VSync 或完整帧时间。单轮有明显波动：例如旧版交替图片 p50 为 2.375–3.842 μs，当前为 2.374–3.352 μs；因此保留逐帧 CSV 和每轮结果，不把上表当作固定加速比例或 ESP FPS。原始证据为 `canvas-reuse-warmed-performance/report.json`。

早期只预热少量帧、每组 2000 样本的 `canvas-reuse-final-performance/` 出现重复图片 p95 中位数约 5.55% 回退，同时旧实现本身在不同轮次的 p50 从约 9 μs 变到 5.6 μs；该失败结果保留。增加持续预热和采样量后，在同一最终代码上重测，五轮未发现可重复的超过 5% 回退。最早的 `canvas-reuse-performance/` 则记录加入相同绑定快速路径前的实现，不能冒充最终版本。

复现命令（在项目根目录执行，基线文件已在本工作区保存）：

```sh
cmake -S deps/pxa-system/libpxa -B build/resource-refactor/canvas-reuse-release \
  -DPXA_BUILD_TESTS=ON -DPXA_BUILD_ADAPTERS=ON -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_C_FLAGS_RELEASE=-O3 -g -UNDEBUG' \
  -DPXA_LVGL_SOURCE_DIR="$PWD/build/simulator/pai-touch/_deps/lvgl_source-src"
cmake --build build/resource-refactor/canvas-reuse-release --target pxa_lvgl_ui_test -j4
python3 tools/compare-canvas-metadata.py \
  build/resource-refactor/canvas-reuse-release \
  build/resource-refactor/canvas-reuse-baseline/pxa_lvgl_ui.c \
  build/resource-refactor/canvas-reuse-reproduction
```

### 最终验证

- `canvas-reuse-checks-final.log` 和 `canvas-reuse-final-logs/`：最终实现的完整一键回归通过，包含 31 项 Core/WAMR、ESP Host、规范/包工具、真实 LVGL 以及 15 组 AOT 运行。所有运行退出预算归零。ASan/UBSan 的常规用例各提交 2000 帧，并新增复用数组/空包装后的部分准备失败、后期 alpha 分配失败、像素引用释放、关闭后拒绝再次提交和当前帧继续绘制检查。
- `canvas-reuse-plane-comparison/report.json`：实际签名 plane-shooter 再次通过 15 项像素相等比较及运行/后台/恢复画面变化检查；五个菜单还与上一阶段 IMAGE_HANDLE 版本逐字节一致。默认/低预算/加载中退出的共享资源预算和 Guest/AOT 大小保持不变，不将该计数器当作包含了新元数据的完整 Host 内存统计。
- `canvas-reuse-sensecap-r4-build.log` → `canvas-reuse-pai-final-build.log`：两板顺序固件构建通过，各通过 85 个 codec 对象和四个强预算入口的审计。实际 Xtensa 布局见 `canvas-reuse-layout.json`：UI workspace 616 B、节点 128 B、命令 52 B、图片包装 64 B、Canvas 64 B、Core Canvas view 96 B、绑定条目 16 B，提交用栈上 swap 152 B。没有连接 ESP 设备，不宣称真机速度或实际栈高水位。
- 两配置的 product/desktop 入口构建通过，日志为 `canvas-reuse-sim-pai-r3.log`、`canvas-reuse-sim-sensecap-r3.log`。源码、固件、日志和预算证据分别归档为 `canvas-reuse-source-identity.json`、`canvas-reuse-firmware-identity.json`、`canvas-reuse-log-index.json`、`canvas-reuse-measurements.json`。
- `canvas-reuse-no-runtime-hash-symbols.json` 再次检查 16 个运行时资源对象，没有 SHA/摘要上下文依赖。安装阶段的验证保留。

完整目标继续执行。普通 UI 树事务的后期失败回滚、旧 UI 图片路径迁移与删除、alpha 合成快照/LVGL 绘制分配、完整内存核算与准入、其余异常/生命周期矩阵、voxel 游戏过程及实际整帧的同画质多轮性能验收仍未完成；本节的提交微基准不替代这些要求。
# UI 结构提交与递归取消修复

本阶段修复三个实际问题：REMOVE 后提交的滚动处理仍访问已释放节点（`ui-transaction-repro.log` 中 ASan 复现）；父节点递归删除后，子节点 CREATE 命令仍持有失效指针；同一事务中创建后删除子树时，Core 用最终虚拟树查找父句柄，导致临时子节点成为未回收的顶层对象（`ui-transient-repro.log` 复现）。

删除和替换的旧节点现在先隐藏，保留句柄、图片和 Canvas 引用，完成布局、滚动和 alpha 准备后才销毁。失败恢复待删除节点原来的可见性，递归清理新树，并保持旧 primary root、透明层像素和 revision。新节点先登记再创建 LVGL 对象，避免初始化部分失败时 DELETE 回调破坏节点链表。Core 回放通过已创建/现存句柄查找父节点，最终被删除的临时节点仍保持正确父子关系。alpha 状态回调在事务准备期间不发布中间输出。

新增 ASan/UBSan 用例覆盖修改子节点后删除祖先、创建再删除嵌套子树、PATCH 删除加创建在 alpha 分配失败后保留旧树、REPLACE_SURFACE 的两次独立 plane 分配失败，以及成功重试和退出清零。失败检查包括旧对象身份、Core 句柄、原画面和透明层指针/内容/revision。此阶段没有覆盖已有节点原地属性、IMAGE_HANDLE、网格及 MOVE 的完整撤销，也没有把适配器分配失败冒充 LVGL 原生堆失败。

`ui-transaction-checks.log` 的完整资源回归通过，包括 31 项 Core/WAMR、ESP Host、规范/打包、LVGL 和 15 组实际 AOT 场景。随后仅调整状态字段位置以复用结构体对齐空隙；最终布局的独立 sanitizer 检查为 `ui-transaction-final-test.log`，两配置的 product/desktop 入口为 `ui-transaction-sim-*-final.log`。`ui-transaction-final-plane-comparison/report.json` 记录最终运行程序的 15 项精确像素比较、战斗暂停/恢复及低预算运行，全部通过；五个菜单也与前阶段 Canvas 复用后的输出完全相同。共享外部预算的默认/低预算峰值仍为 250754/179138 B，不能据此推断完整原生内存或帧时间。

Sensecap Watcher（IDF 5.5.4/GCC 14）的最终构建完成后才启动 Pai Touch（IDF main/GCC 16）。最终日志为 `ui-transaction-sensecap-final-build.log` 和 `ui-transaction-pai-build.log`，均通过 85 个 codec 对象及四个预算入口检查。实际 Xtensa 布局仍为 UI workspace 616 B、节点 128 B、命令 52 B；没有新增事务专用堆分配。首次 Sensecap 构建的 624 B workspace 版本保留记录，但最终产物使用对齐空隙后的 616 B 布局。源码、对象、布局、日志及 AOT 回收记录汇总于 `ui-transaction-report.json`。

完整目标仍未完成。下一步继续 IMAGE_HANDLE 等已有节点修改的所有权回滚，以及原生分配核算、旧 UI 资源路径迁移、完整生命周期矩阵与真实帧时间验收。运行时 SHA 和安装后修改检测不在目标范围内。

## UI 图片源所有权回滚

在结构事务修复之后，进一步复现了 IMAGE_HANDLE 提交失败时旧资源已经释放的问题：`ui-image-undo-repro.log` 的旧引用/源描述符断言失败。基线源码保存在 `ui-image-undo-baseline/`，包含此前结构修复后的实际适配器、Core 事务和测试文件。

- IMAGE_HANDLE、尚未迁移完的 ASSET 及两者的 CLEAR 统一进入图片源所有权交换。准备的新资源与节点原引用交换，旧引用保留在事务命令中；成功后才释放旧资源，失败则按命令逆序交换回来并释放未提交的新资源。没有回读文件或重新获取已关闭的 Guest 句柄来补救回滚。
- CREATE 与图片修改使用互斥的所有权字段。没有新增撤销专用堆分配；实际 Xtensa 命令从 52 B 变为 56 B，UI workspace 616 B、节点 128 B、图片包装 64 B、Canvas 64 B 均不变。新增成本为每条事务命令 4 B，随事务释放，不能把它称为零成本。暖态 Canvas 的两个工作负载仍各有零次适配器/Core 分配，驻留量保持 4240/13768 B；没有重新宣称本阶段带来帧时间收益。
- 旧 ASSET 解析移到 LVGL execute 之外，避免外层 GUI 锁覆盖其读取过程；解析失败返回资源错误，保留原图。它仍是同步旧接口，应用迁移和移除任务没有取消，也不作为新异步资源 API 的替代。
- 最终 sanitizer 用例覆盖 Guest 先关闭旧句柄、单次替换、CLEAR、连续 SET/CLEAR/SET、ASSET 与句柄混用、晚期 alpha 分配失败、资源解析失败和成功重试。失败检查旧描述符身份、资源引用数、透明层指针/内容/revision及临时节点回收；退出时图片对象和所有旧路径解析/释放计数配平。

`ui-image-undo-checks.log` 的完整一键回归通过，`ui-image-undo-final-logs/` 归档 36 份日志，包含 31 项 Core/WAMR、ESP Host、规范/打包、LVGL 及 15 组实际 AOT 场景；这些 AOT 运行退出后的共享预算全部归零。`ui-image-undo-plane-comparison/report.json` 的 15 项精确像素比较与战斗暂停/恢复检查通过，五个菜单也与前阶段一致。默认/低预算共享外部峰值仍为 250754/179138 B；该数值不包含完整 LVGL 原生堆，不能用来宣称总内存上限或下降。

两配置 product/desktop 入口重建通过。Pai Touch 构建完成后才启动 Sensecap Watcher，分别见 `ui-image-undo-pai-build.log` 与 `ui-image-undo-sensecap-build.log`，两者均通过 85 个 codec 对象及四个预算入口检查。源码、目标文件、编译命令、实际布局、日志和回收数据汇总于 `ui-image-undo-report.json`。这是无硬件条件下的模拟与构建证据，尚无 ESP 实测性能结论。

整体目标保持未完成。图片源撤销不等于所有 UI 属性的完整事务性：网格准备失败仍可能被静默忽略，已有文本/样式/尺寸和 MOVE 的撤销、LVGL 原生分配失败与预算核算仍需继续；其他旧 UI 资源路径迁移、完整生命周期矩阵、voxel 游戏过程以及同画质多轮 CPU/帧时间验收也继续保留。运行时 SHA 与安装后修改检测没有恢复。

## 2026-09-28：Pai Touch / ESP32S31 Korvo-1 实机验证

用户提供两块设备后，完成原始 Flash 备份、应用分区刷写、真实资源循环和游戏验证。详细结果与阶段边界见 [实机验证报告](pxa-hardware-validation-20260928.zh-CN.md)，原始证据位于 `build/resource-refactor/hardware-20260928/`。

实机发现并修复 KV 首次启动父目录缺失、被动状态更新重置 UI、大型光照调色板挤占内部 SRAM，以及 S31 IMAGE 缓冲区未满足 LVGL 64 B 对齐的问题。Korvo 接入共用音频输出，并增加 S31 codec 构建审计。没有新增运行时 SHA 或安装后修改检测。

Pai 最终固件三轮资源场景完成时间为 23.596/23.561/23.603 秒；Korvo 在最后调色板/IMAGE 修改前完成三轮 28.917/28.947/30.464 秒，两设备各轮音乐 underruns/missing 均为 0。Korvo 最终固件另用 plane-shooter 验证文件图片、进入战斗、后台恢复与退出。Pai voxel 修复后进入实际地形游戏，当前 296×240 场景约 16 FPS，累计 dropped=1；这不是与旧版比较的性能提升结论。最终两设备均响应，应用已退出，原有用户应用保留。

最终完整资源回归通过（36 份日志、31 项 Core/WAMR、ESP Host、规范/包工具、LVGL sanitizer 和 15 组 AOT）；三板固件顺序构建通过，Pai/Sensecap 两配置的 desktop/product 入口重建通过。Sensecap 没有本轮硬件证据，Korvo 早期资源循环不能标注为最终 ELF 测试。

整体目标仍未完成。完整 UI 事务、旧路径迁移、原生预算核算、异常和熄屏/睡眠矩阵、同画质多轮真实整帧性能验收继续保留。实机资源计数、聚合 free_heap 与完成时间不能替代这些要求。

## UI 网格准备、单位与 MOVE 回滚

本阶段沿用上一轮实机修复后的工作树。`ui-grid-move-baseline/` 保存修改前的 LVGL 适配器与测试；`ui-grid-move-repro.log` 记录旧实现把网格分配失败当成提交成功的断言失败。另一个复现 `ui-grid-units-repro.log` 证明固定轨道的 1/64 dp 被直接当作像素：17 dp 变成 1088 像素。

网格描述符现在在 execute 之前准备，第一轴或第二轴分配失败都会取消整笔提交并释放已准备的数据。执行时交换指针、持有旧描述符，直到提交成功才释放；失败逆序恢复原指针、轴状态和 layout。CLEAR 现在真正移除对应轴，并停用网格布局；另一轴保留，重新设置后可恢复网格。固定尺寸按 Surface density 换算。

Core 与 LVGL 共用轨道报文校验，限制 1–64 条、合法类型、零保留字节、固定尺寸 0–INT32_MAX；fraction 权重统一为 1–99，避免 LVGL 的 100 权重与模板结束标记冲突。Guest SDK 同步拒绝无效权重和尺寸；网格 cell 的两个 span 必须非零。规范同步写明单位、范围和 CLEAR 语义。

MOVE 记录操作发生前的父对象和兄弟索引。透明层准备失败时先逆序恢复 MOVE，再恢复图片与网格引用，最后删除新建对象，避免把移入新子树的已有节点一起销毁。另修复同父节点移动到后方兄弟之前时的索引偏移。撤销字段复用命令的所有权 union，没有单独撤销堆对象；旧、新轨道在提交期间会同时存活，必须计入峰值，不能称为零内存开销。

回归覆盖两个独立准备失败点、连续 SET/CLEAR/SET、行/列清除、同节点多次 MOVE、节点移入新子树后的失败、精确兄弟顺序、Core 父句柄、旧页面/透明层像素和 revision、预算回到原驻留量、成功重试及退出清零。Core/SDK 另测 0/1/99/100/UINT32_MAX 权重边界、超长轨道、保留字节、非法类型和零 span。

最终完整一键回归 `ui-grid-move-r2-checks.log` 通过，36 份日志归档于 `ui-grid-move-r2-final-logs/`，覆盖 31 项 Core/WAMR、ESP Host、规范/包工具、LVGL sanitizer 和 15 组 AOT 场景。`ui-grid-move-r2-plane-comparison/report.json` 的 15 项真实 plane-shooter 像素比较及战斗暂停/恢复检查通过；默认/低预算共享外部峰值分别为 251895/179242 B，退出归零。该预算不含完整 LVGL 原生堆；与较早阶段的差异还包含上轮 IMAGE 对齐开销，不能归因于本轮事务修改。暖态 Canvas 的两个用例仍各为零次适配器分配、驻留量 4240/13768 B，本轮没有重新宣称 CPU 或帧时间收益。两配置的 product/desktop 入口最终重建通过，见 `ui-grid-move-r2-sim-pai.log` 与 `ui-grid-move-r2-sim-sensecap.log`。

最终固件按 Sensecap → Pai → Korvo 顺序构建通过，日志为 `ui-grid-move-r2-{sensecap,pai,korvo}-build.log`，均通过对应 codec 对象和四个强预算入口审计。三者实际目标文件均为 UI workspace 616 B、节点 128 B、命令 56 B、图片包装 64 B。最终源码保存在 `ui-grid-move-final-sources/`，固件 bin/适配器与 Core 解码目标文件/配置/审计结果保存在 `ui-grid-move-final-firmware/`；`ui-grid-move-report.json` 记录源码、产物、编译命令、日志和像素结果，可由 `ui-grid-move-finalize.py` 在本工作区重建。这一阶段只构建固件，没有再次刷写，两台设备上一节的硬件结果不能标成当前补丁的实机结果。

本节不把 alpha 准备失败的回滚视为全部 LVGL 原生 OOM 已解决：原生 reparent 会重分配 children 数组，仍需纳入后续预算与分配失败处理。已有文本/样式/尺寸/GRID_CELL 等普通属性撤销、完整原生内存核算、旧 UI 资源路径迁移、异常/睡眠矩阵及多轮整帧性能验收仍未完成；没有恢复运行时 SHA 或安装后修改检测。

## 普通 UI 属性的失败回滚

上一阶段的网格与 MOVE 修复仍保留。本阶段在 `ui-properties-baseline/` 保存适配器和测试源码，`ui-properties-repro.log` 复现文本/宽度已经改变、透明层准备失败后返回资源限制却没有恢复旧文本的问题。

现在对已有节点的已实现普通属性建立临时撤销记录，保存受影响的本地样式值及其是否存在、节点标量状态、控件值、对象标志和滚动状态。文本按实际长度复制旧值，支持连续 SET/CLEAR/SET；输入框恢复光标和文本选择范围，下拉控件恢复选中项，进度条恢复范围及被新范围钳制的值。回滚保留本地样式的缺省状态，不把主题继承值变成永久本地覆盖。POSITION 恢复原兄弟顺序。先恢复暂时删除标记再逆序撤销属性，避免失败的 REMOVE 把已经恢复的可见性覆盖回中间值。指针事件订阅关闭或回滚时移除对应回调，避免残留注册。

新节点失败时直接销毁，不分配这些记录；IMAGE/ASSET、网格和 MOVE 继续使用已有所有权机制。元数据在 execute 前分配，旧文本在 GUI 执行锁内读取和暂存；暂存失败也会撤销此前已经执行的属性，释放全部准备数据。成功或取消后销毁撤销记录，不新增永久属性缓存。

这项正确性修复有临时内存成本。三板目标文件均显示 UI workspace 616 B、节点 128 B、命令 56 B 保持不变，新增撤销头为 132 B，相关样式条目在 S31 实测为 16 B/项，另加旧文本长度与结尾零字节。混合属性测试的共享 Core/适配器测试分配器原驻留为 9852 B：中途文本暂存失败、后期透明层失败、属性后 REMOVE 再失败三种情况峰值分别为 21130/57427/57524 B，退出该次失败提交后都回到 9852 B。峰值包含命令准备和透明层 staging，不能把差值全部归因于撤销记录，也不包含 LVGL 原生堆；这里没有总内存下降或帧时间改善结论。

`ui-properties-checks.log` 的完整资源回归通过，36 份日志保存在 `ui-properties-final-logs/`，包含 31 项 Core/WAMR、ESP Host、规范/包工具、LVGL sanitizer 及 15 组实际 AOT 场景。随后补充虚拟列表缩小内容后失败、恢复内容高度与滚动位置，以及分配峰值输出，最终独立 sanitizer 检查为 `ui-properties-final-lvgl-test.log`，生产代码没有在完整回归后改变。测试逐项核对 31 个样式字段的值及本地覆盖状态，并检查原画面、透明层指针/像素/revision、Core 事件掩码、成功重试和退出清零。暖态 Canvas 两组测试仍各为零次适配器/Core 分配，驻留 4240/13768 B。

`ui-properties-plane-comparison/report.json` 的 15 项真实 plane-shooter 像素比较和战斗暂停/恢复检查通过；默认/低预算共享外部峰值为 251895/179242 B，退出归零，该计数不能代表完整 UI 原生内存。Pai/Sensecap 两配置 product/desktop 入口均重建通过。最终固件按 Korvo → Pai → Sensecap 顺序构建，通过对应 codec 与强预算入口审计；本阶段没有再次刷写，上一轮设备结果不作为此补丁的实机验证。源码、固件与对象文件、配置、编译命令、布局和原始日志身份汇总在 `ui-properties-report.json`，可由 `ui-properties-finalize.py` 在本工作区复查；最终源码/产物另存为 `ui-properties-final-sources/` 和 `ui-properties-final-firmware/`。

整体目标继续保持未完成。这些回归覆盖了列出的属性及失败组合，不是所有 UI 属性组合和正在进行的动画/交互状态的完备证明。LVGL setter 和回滚内部仍可能重新分配，部分原生 setter 不返回 OOM 状态；原生堆失败的事务性、完整分配核算及准入仍必须继续完成。旧 UI 资源路径迁移、完整异常/熄屏/睡眠矩阵、真实游戏同画质多轮 CPU/整帧性能验收和最终逐项审计也未取消。运行时 SHA 与安装后修改检测没有恢复。


## 透明层 snapshot 缓冲复用与局部上限

本阶段移除适配器每次透明层刷新调用 `lv_snapshot_take` 的路径，改用栈上 descriptor 和通过适配器分配器记账的对齐 scratch，再调用同步的 `lv_snapshot_take_to_draw_buf`。容量足够时复用；扩容先准备候选，成功后交换，失败释放候选并保留旧画面。没有透明层或 reset 时释放 scratch。新增 `snapshot_limit_bytes` 与 `alpha_limit_bytes`，零值按主显示尺寸计算默认上限；前者包括对齐填充，后者限制单份 RGB565+A8 plane。旧/新缓冲在提交期间可能重叠，这些局部上限不是全局原生堆准入。保留 scratch 会增加透明层存在期间的稳态驻留，不能把减少分配次数称为总内存下降。

在提前显示透明节点以计算 snapshot 时，还发现 MOVE 失败回滚后节点可能继续透明。新增节点状态跟踪，在节点退出透明子树和撤销后恢复颜色及可见性。三板 DWARF 实际布局显示 workspace 从 616 B 增至 632 B，节点仍为 128 B，命令仍为 56 B。LVGL 原生 draw-task、style/setter 分配仍存在，尚未被该 scratch 记账或局部上限覆盖。

`ui-snapshot-baseline/native-calls-repro.log` 用修改前适配器复现 100 次暖态刷新调用 100 次原生 snapshot 创建，候选的 `ui-snapshot-final-lvgl-test.log` 为 0 次、0 次新增适配器分配；画面 hash 同为 7879175267504642313。早期 `ui-snapshot-repro.log` 错把刻意不同颜色的帧作基线，不能用于此结论，保留原始记录。回归还覆盖 scratch 上限、plane 上限、候选分配失败、成功重试与透明层移除释放。三种失败保留旧 root、像素、revision 和当前分配量；16×16→24×24 重试成功，改回不透明内容释放 4035 B，deinit 归零。普通属性回滚测试当前驻留 23495 B，三个峰值为 34773/71070/71167 B，包含新 scratch，不包括 LVGL 原生堆。

`ui-snapshot-checks.log` 完整资源回归通过，36 份日志在 `ui-snapshot-final-logs/`，覆盖 31 项 Core/WAMR、ESP Host、规范/包工具、LVGL sanitizer 及 15 组 AOT。真实 plane-shooter 的 15 项像素比较通过，见 `ui-snapshot-plane-comparison/report.json`；共享外部资源峰值默认 251895 B、低预算 179242 B，不代表完整 UI 内存。Pai/Sensecap 的 product/desktop 入口构建通过。Sensecap→Pai→Korvo 顺序固件构建通过，包含 codec 预算审计；源码、固件、对象、配置、布局和日志身份由 `ui-snapshot-finalize.py` 汇总为 `ui-snapshot-report.json`，实机运行另记，不能沿用旧固件证据。

Canvas 提交微基准脚本隔离了后加的事务回归，保留相同的两个计时循环、像素和引用/释放检查，完整事务测试仍独立执行。`ui-snapshot-canvas-preflight-r2`（并行构建）和 `ui-snapshot-canvas-performance`（无构建）两组五轮测试分别在重复图片的 p50/p95 超过 5% 门槛，原始失败均保留。扩大到 15 轮的 `ui-snapshot-canvas-performance-15` 同门槛通过：普通提交 p50/p95 比值 0.854/0.810，重复图片 0.874/0.937；同机频率/调度波动明显，运行期间有串口备份，不把这一次通过视为整帧性能或 ESP 性能验收。两种工作负载始终保持零暖态适配器分配、像素一致。后续仍须完成受控整帧测量；没有放宽门槛、删掉失败结果或恢复运行时 SHA。


### 本阶段固件的后续实机验证

两块设备重新完整备份并核对分区表后，仅更新应用分区。Pai 三轮资源场景通过，耗时 23.565/23.465/23.467 秒，音乐 underruns/missing 均为 0；退出后 free_heap 7231575/7231307/7231659 B。Korvo 现有 plane-shooter 与 voxel 分别完成活动战斗/地形渲染、约 15 秒后台恢复和退出；voxel 音乐 underruns/missing 为 0。Korvo 安装资源循环应用遇到明确的 LittleFS 空间不足，没有删除原有应用，也没有将本轮安装失败标成运行通过。最终设备均响应，测试应用均 inactive。

详细阶段边界、原始失败、日志丢失、固件身份和测试限制见 [最新 UI 固件实机报告](pxa-hardware-ui-snapshot-20260928.zh-CN.md)，证据为 `ui-snapshot-hardware/report.json`。本次不替代同画质受控性能比较、完整原生内存核算或熄屏/睡眠验收。整体目标继续保持未完成。


## 多透明层的暖态缓冲复用

上一轮已消除 snapshot 像素缓冲的重复分配，但多个透明层仍在每次刷新时新建 RGB565/A8 staging plane。本阶段保存 `alpha-staging-baseline/`，用最终同一工作负载链接旧适配器，`warm-allocations-final.log` 复现连续 100 次相同画面刷新产生 200 次适配器分配。

现在多透明层使用两份 plane 交替提交。所有 snapshot 完成后才交换；第一或后续 snapshot 返回失败时，已发布的像素指针、内容、revision 和 Canvas 所有权不变。同尺寸缓冲可以跟随整体位置改变，避免移动引起重新分配。尺寸变化在实际准备分配的位置先释放闲置 staging 缓冲，避免同时保留第三份 plane；失败可以回收这份私有缓存，但不能回收已发布的输出。失败中新分配的 staging plane 会释放，已经存在的同尺寸缓冲可保留供重试。切换回单透明层、无透明层或 reset 时释放备用 plane。

这项优化以驻留换取分配次数，不能称为总内存下降。在共享 Core/适配器测试分配器的相同工作负载下，100 次刷新从 200 次分配降为 0，像素 hash 均为 4637473789383918359。旧/新暖态驻留为 6347/7595 B，增加 1248 B；刷新窗口峰值都为 7595 B。这个计数不含 workspace 自身及 LVGL 原生堆。三板 DWARF 显示 workspace 从 632 B 增至 672 B，节点仍为 128 B、命令仍为 56 B。每份 plane 受原来的 `alpha_limit_bytes` 约束，两份 plane 都经过适配器 allocator 记账，但这仍不是完整原生内存的全局准入。

最终 sanitizer 回归在 `alpha-staging-final-test.log`：除 100 次零分配刷新外，还检查 20 次整体位置往返、第一/第二个 snapshot 返回失败、几何变化的两次独立 plane 分配失败、扩展区域已经绘制后的 snapshot 失败、成功重试恢复驻留，以及之后无透明层和 deinit 的回收。故障注入通过链接包装使真实 LVGL snapshot API 返回失败，不等于在 LVGL 内部所有 malloc 点注入 OOM。早期测试试图直接设置受 flex 管理对象的 x，被布局恢复为 0，因此并未制造尺寸变化；`alpha-staging-resize-test.log` 保留此失败，最终改为真实影响布局坐标的 translate_x。

`alpha-staging-final-checks.log` 的完整资源回归通过，36 份日志保存在 `alpha-staging-final-logs/`，覆盖 31 项 Core/WAMR、ESP Host、规范/包工具、LVGL sanitizer 和 15 组 AOT。`alpha-staging-final-plane-comparison/report.json` 的 15 项真实 plane-shooter 像素比较通过；默认/低预算共享资源外部峰值仍为 251895/179242 B，不代表完整 UI 内存。Pai/Sensecap 的 product/desktop 入口均重建通过。最终固件按 Pai→Korvo→Sensecap 顺序构建，并通过对应 codec 预算审计；本阶段没有再次刷写，上一节硬件结果属于之前的 ui-snapshot 固件。

源码、旧适配器复现、编译参数、目标布局、固件和日志由 `alpha-staging-finalize.py` 汇总到 `alpha-staging-report.json`。没有新增 CPU/整帧性能提升结论，也没有恢复运行时 SHA。LVGL 原生 draw-task/style 分配、原生 OOM 事务性与完整预算核算仍待完成。旧 UI 路径的后续迁移清单保存在 `alpha-staging-legacy-inventory.json`：weather、Store、arcade 分别仍有 1/3/39 个旧接口源码调用点；这是静态清单，不是迁移完成证明。完整异常/睡眠矩阵、同画质整帧性能验收和最终逐项审计继续保留，整体目标未完成。


## Store 的异步 IMAGE 迁移与验证

保存当前工作区 Store 的完整修改前源码和签名模拟器包到 `store-image-baseline/`，没有用 Git HEAD 覆盖已有 Store 变更。将唯一 UI 搜索 PNG 移到构建输入 `resources/search.png`，由 `resources.json` 离线生成 BGRA8888 PXR1。三个 UI 调用点共用一个 IMAGE 句柄，启动只提交一次 LOAD，普通事件分发处理完成；暖态切换页面及后台/前台复用同一图像。加载中退后台取消请求，处理取消与成功完成的竞态；停止后不再启动请求，晚到的成功句柄关闭。普通失败等待显式刷新或前台恢复重试，不形成每帧重试循环。加载期间和失败时保留图标布局，Store 仍可操作。launcher 的 `assets/icon.png` 不属于这三个 UI 调用点，仍保留。

`store-image-final-comparison/report.json` 使用同一个实际 product Host 分别运行旧、新签名 AOT；网络后端固定返回不可用，避免在线目录变化污染像素比较。首页、搜索页、返回首页、再次搜索、后台恢复五个画面在普通及延迟加载模式下共 10 项新旧逐字节比较通过；四种可交互模式的 12 项往返/恢复画面比较也通过。旧路径累计查询 9 次，新包为 0 次。100 ms 分块读取延迟下的后台取消/恢复、加载中退出以及 4 KB 图片预算失败均通过；退出后的共享资源计数全部归零。单元 sanitizer 覆盖完整 64 位 token、重复请求、取消/成功竞态、错误类型、立即拒绝、显式重试、暖态复用、停止后晚到成功与不再重启。

这次迁移尚不是总内存优化。固定同一工作负载下，旧包共享资源外部峰值 3468 B，新包 41016 B。旧路径只把编码 PNG 记在该预算内，LVGL 原生解码结果不在其中，因此不能直接用差值比较完整像素内存。新包一份最终图像计账 9327 B，但模拟器同时预留 Assets metadata 31321 B，启动原生 worker 后还占 131072 B 栈；栈不包含在上述共享预算里。新包 Guest 线性内存仍为 196608 B，AOT buffer 243708→247420 B。元数据数字含路径长度等开销，不能与不同绝对路径运行的计数混用。后续必须优化小资源集合的固定元数据/任务成本；这里不声称 CPU、FPS 或总内存改善。

`store-image-checks.log` 完整回归通过，44 份日志保存于 `store-image-final-logs/`，包含 31 项 Core/WAMR、ESP Host、规范与包工具、LVGL sanitizer 和增加 Store 后的 19 组实际 AOT 场景。`tools/test-pxa-resources.sh` 已纳入四组 Store AOT 和两个生命周期控制器执行模式；`tools/compare-store-images.py` 可复现独立旧包像素/预算对比。Store 的 ESP32-S3、ESP32-S31 AOT 包均构建成功。本次生产变更限于 Guest，Host 生产代码未修改；没有重新刷固件。运行时 SHA 校验或安装后修改检测没有恢复。

### Store 的本轮实机验证

Pai `/dev/ttyACM0` 与 Korvo `/dev/ttyUSB0`、2000000 baud 使用独立测试 ID `pxa-img`，Guest 主代码与候选 Store 相同，仅修改包 ID、显示名和 i18n namespace，保留原装 Store 0.8.0 和其数据。两块设备均安装成功；首页搜索按钮、搜索框图标和空搜索提示的截图已人工检查，日志确认各次启动图像 READY。后台 15.008/16.039 秒后恢复搜索页，未再次加载图像；随后两板分别完成三轮启动/退出。Pai 三轮退出 free_heap 为 7193527/7193523/7193531 B，Korvo 为 12009727/12009723/12009743 B。这是暖态短测的聚合堆数，不证明所有生命周期无泄漏，也不是新旧内存对比。最初带截图、权限弹窗的运行曾改变系统驻留，不能把暖态测量代替完整内存时间线。

两板捕获的 Assets 统计均为图像缓存外部峰值 9303 B、metadata 29251 B、task_stack 6144 B。进一步确认单张图的管理成本偏大，后续优化需要包含这些成本。实机使用当时已有固件，版本字符串为 `e6bb9d9-dirty`，本阶段没有重刷或读回固件二进制；上一次记录的刷写属于 ui-snapshot，版本字符串本身不能唯一证明二进制身份，不能将本轮当成后加 alpha-staging 补丁的实机验收。

首次长 ID `pxa-store-resource-check` 在两板安装会话目录创建时返回 `ENAMETOOLONG`（errno=91），尚未启动应用。改为短 ID 后成功，原始失败记录没有删除。Korvo 第一轮截图因串口块偏移缺失失败，脚本停止应用，加入截图重试后重新完成；日志中存在 DROP 标记，不能据此声称没有其它未捕获日志。Pai 有已有 UART FIFO overflow/ADC timeout 日志，本次没有定位或修复这些独立问题。最终卸载了本轮独立测试应用，删除了本轮失败上传，现有应用列表保持原状且均 inactive；Korvo 原有的 staged Store 和其它上传文件保留。Pai 的 inbox 查询返回 path_not_found，Korvo 列表不含本轮测试包。

证据汇总为 `store-image-report.json`，包含源码/产物身份、44 份回归日志、像素比较、实机生命周期、三轮堆统计及限制；可运行 `store-image-finalize.py` 重新汇总。截图与原始日志在 `store-image-hardware/`。完整目标仍未完成：单图固定成本优化、weather/arcade/common 的旧图片路径迁移、LVGL 原生分配与 OOM 原子性、完整预算时间线、熄屏/睡眠矩阵、同画质游戏 CPU/整帧性能对比与最终逐项审计继续保留。


## 缓存条目借用不可变目录，移除重复元数据

上一轮 Store 实测确认固定元数据成本偏大。本阶段保存旧 `asset_cache.c/.h`、单元测试、sanitizer 库和真实 Store Host 到 `cache-metadata-baseline/`。没有按目录条数缩减缓存/请求容量，因为未消费的失败结果、取消中的旧工作及不同 owner 可以同时占用同资源的多个条目。

现在每个条目复制 `pxa_asset_info_t` 标量和包命名空间，借用已安装 catalog/manifest 的不可变路径、file record 与摘要，移除槽位内最大路径数组、file record 和摘要副本。原有去重键、owner 配额、独立 ticket、取消、预取、失败和像素对象生命周期保留。Host cache API 明确要求目录存储至少活到所有任务完成并 drain；临时 lookup-info 结构本身不必保留。两端 worker 原本已在 shutdown/drain 后才释放目录，像素对象没有借用目录指针，因此渲染中不会增加查询或锁。不存在运行时重算 SHA；去重比较使用已有的安装元数据。

相同容量（64 entries / 48 requests / 每 owner 32）的原生 Host 工作区从 30167 B 降到 9687 B，减少 20480 B。旧库和新库链接同一最终测试工作负载均通过：100 次场景切换的资源峰值仍为 393408 B，loads/frees 均为 407；默认小测试工作区 7895→2775 B。新增一资源 48 个失败结果同时保留、配额/全局满、关闭后重试、255 字节目录路径、临时 lookup 被覆盖、shutdown 等待在途工作测试，证明没有靠缩小容量获得收益。`cache-metadata-baseline/capacity-test.log` 与 `cache-metadata-capacity-test.log` 保存原始结果。

`cache-metadata-store-comparison/report.json` 让保存的旧 Host 与候选 Host 加载同一份 Store IMAGE 签名包，10 项新旧逐字节画面比较、12 项往返/恢复比较通过。两版 metadata 为 31321/10841 B，共享资源外部峰值为 41016/20536 B；预算失败场景仍可返回并操作 UI，退出全部清零。图像载荷、Guest 线性内存、AOT buffer 和 worker stack 不变，128 KiB 模拟器原生栈仍不包含在共享预算里。本阶段未做 CPU/整帧性能提升结论；此处只能说明被测缓存元数据和共享预算下降，不能替代完整内存时间线或总体目标验收。

### 完整回归、三板构建与本轮硬件结果

`cache-metadata-checks.log` 完整资源回归通过，44 份原始日志已保存到 `cache-metadata-final-logs/`，包含 31 项 Core/WAMR、ESP Host、包/规范、LVGL sanitizer 及 19 组实际 AOT 场景。Pai/Sensecap 两配置的 product/desktop 入口均构建通过。三板最终固件与 codec 预算审计通过，最终日志分别为 `cache-metadata-pai-build.log`、`cache-metadata-korvo-build.log`、`cache-metadata-sensecap-idf55-build.log`。

构建环境需要明确区分：Pai `build/firmware/canvas-image-pai`、Korvo `build/firmware/esp32s31-korvo-1` 使用 main IDF/GCC 16；Sensecap `build/firmware/sensecap-watcher` 使用 `/home/lucinhu/esp/esp-idf/esp-idf-v5.5.4/export.sh` 的 IDF 5.5.4/GCC 14。本轮最初误用 main 环境触发 Sensecap 旧工具链缓存检查失败，随后隔离的 main 配置尝试被停止，最终切回原有 5.5.4 构建通过。保留两个不成功的尝试，不把它们算作通过，也不将不同工具链的二进制大小差异算作优化收益。

两块设备各完成新的 16 MiB Flash 备份，核对分区表后仅刷写 `0x10000` 应用分区，esptool 写入校验通过。备份逐字节确认 Pai 原应用对应 ui-snapshot，Korvo 原应用对应 alpha-staging；因此不能把两板旧固件描述为同一个阶段。新二进制分别为 3264336/3590544 B，完整身份在 `cache-metadata-report.json`，Sensecap 构建产物为 3281136 B。本阶段两板实际运行了新 Host，包含此前尚未统一刷入两板的透明层变更。

使用上一阶段原样保存的 `pxa-img` 签名 Guest 测试包，两板均完成三轮图像 READY、停止与再次启动。两板 Assets metadata 都从旧实测 29251 B 降为 9283 B，减少 19968 B；图像缓存外部峰值仍为 9303 B，task_stack 仍为 6144 B。这里比较的是明确的元数据计数，不把系统 free_heap 差值全部归因于本补丁。Pai 三轮退出 free_heap 为 7273407/7273331/7273399 B，Korvo 为 12158051/12158055/12158051 B。Korvo 搜索页图标截图正常，后台约 16 秒后恢复，期间没有第二次图像 LOAD 完成日志。

Pai 原有 resource-scenes 又完成三轮，每轮 100 次场景切换、20 张纹理中四张可见、100 次准备好的音效触发及并行音乐；耗时 23.560/23.672/23.458 秒，三条音乐累计统计均为 underruns=0、missing=0。退出 free_heap 为 7259743/7259315/7259307 B，属于系统聚合值和有限轮次观察；没有把它当作所有原生内存无泄漏证明，也没有通过固定时长场景测试宣称游戏帧率提高。

原始异常也保留：Pai 刷写后的首次 INFO、安装 HELLO 和 doctor 未响应；额外复位后启动日志确认 runtime/PXADB 就绪，重试成功，未确定初次不响应的原因。实机日志仍有 DROP 与已有 UART FIFO/ADC 警告，不能宣称日志完整或这些问题已修复。清理测试包后，Korvo 首次列表暂未包含内置 Maze Evil，后续再次读取恢复为原有完整列表，首次和第二次结果均保留；没有执行 Maze Evil 的删除或恢复。最终两板原有包列表与刷写前一致，原有 Store 0.8.0 和 staged 文件保留，本轮独立测试包已卸载，全部应用 inactive。

`cache-metadata-finalize.py` 汇总源码、旧库复现、签名包同源对比、三板产物/构建参数、备份/刷写、设备循环和原始失败为 `cache-metadata-report.json`。源码快照在 `cache-metadata-final-sources/`，固件与对象在 `cache-metadata-final-firmware/`，设备证据在 `cache-metadata-hardware/`。完整目标继续保持未完成：weather/arcade/common 旧路径迁移、LVGL 原生堆与 OOM 事务性、完整内存时间线、实际睡眠/唤醒、同画质 CPU/整帧多轮性能和最终逐项验收仍待完成。


## Weather 图片迁移、共享 Guest helper 与资源成本复核

本阶段保存 Weather、Store 和 plane-shooter 的当前完整源码于 `weather-image-baseline/source/`，并保存 Weather 旧 PNG 签名 AOT 包于 `weather-image-baseline/packages/`。Weather 的 12 张 UI PNG 源图移至 `resources/`，包中由 `resources.json` 离线转换成 PXR BGRA8888；启动图标仍保留原样。Weather 用 IMAGE 句柄显示天气、位置、时间和操作图标，只有 UI 提交成功后才选择所需集合并释放不用的句柄。图片未就绪或预算不足时保持原布局与天气数据，显式刷新或前台恢复允许重试；后台取消在途请求，退出关闭句柄。Store 和 plane-shooter 的重复控制逻辑改用同一个 `pxa_image_set.h`，保留各自页面策略。helper 借用调用方的静态路径和句柄数组，最多 64 项、一次只提交一个加载、没有额外事件队列或像素缓存；完整 64 位 token、取消/迟到成功、独立集合、失败重试、异常 kind 和退出均有原生回归。`pxa_core.h` 使用独立的 v1 状态常量，避免与 Host 原生 `pxa/status.h` 重定义。

可重复的离线实包对比命令：

```sh
python3 tools/compare-weather-images.py \
  --runner build/simulator/product-dev/pxsys_weather_images_test \
  --baseline build/resource-refactor/weather-image-baseline/packages/pxa-weather \
  --candidate build/resource-refactor/weather-final-packages/pxa-weather \
  --key local/pxa-apps/.dev-signing/publisher-public.pem \
  --output build/resource-refactor/weather-image-final-comparison
```

`weather-image-final-comparison/report.json` 记录签名包 manifest SHA、AOT、逐屏内存与命令，原始日志和 296×240 BGRA 截图同目录。旧/新 AOT 为 106660/111264 B，容器为 140745/150426 B；旧包 94 次旧资源路径查找，新包为 0。固定 IP、天气、空气质量和历史响应下，首屏、列表中段和底部 3 个滚动位置，在正常加载与后台延迟取消/恢复两种新包场景中，与旧包共 6 张截图逐字节相同。低 IMAGE 预算 4096 B 时图标载荷为 0、错误有界且天气数据可见；加载中退出后共享预算归零。24 项 Core 测试、Guest SDK/App 测试、资源编译器 9 项测试、Weather provider 测试和 image-set ASan/UBSan 均通过。模拟器场景没有证明真实网络时延、设备 FPS 或总体 RAM 下降。

这里出现了明确的内存回退，不能把图片迁移本身称作内存优化。相同首屏时共享资源外部同时驻留从旧 PNG 路径的 22570 B 增到 IMAGE 句柄的 127086 B；相同场景峰值从 41531 B 增到 127086 B。图像载荷驻留从 22570 B 增到 115465 B，Guest 线性高水位从 98304 B 增到 102400 B；UI 记账当前值同为 10536 B。Linux `mallinfo2().uordblks` 从 545968 B 增到 657712 B，但它混合了 WAMR、LVGL、libc 和其它分配，不能当作 ESP 的内外 RAM 总量。旧路径 PNG 的驻留/解码成本未被共享预算完全覆盖，仍需完整原生内存时间线和同画质性能测量才能判断整个 UI 的取舍。曾试验无损 INDEX8（这 12 张图均不超过 256 种 RGBA 色）：资源载荷降到约 36.5 KiB，但 LVGL 软件绘制对缩放图的逐行解码不能保持完整图像，像素验收失败，相关实现已撤回；`weather-indexed-packages/` 和失败截图保留供后续设计参考，不能作为完成的功能。

设备验证使用独立 ID `pxa-wimg`、两架构签名测试包 `weather-image-hardware-packages/pxa-pxa-wimg.pxa`，运行在上一阶段已核实的缓存元数据固件上。本阶段没有重刷固件。Pai `/dev/ttyACM0` 显示实际天气和雨云图标，首次运行逐项授权四个网络来源，HOME 后再次启动仍正常；Korvo `/dev/ttyUSB0` 以 2000000 baud 显示晴夜月亮图标，HOME 后再次启动仍正常。截图与 PXADB 原始记录在 `weather-image-hardware/{pai,korvo}/`。停止后系统聚合 free_heap 分别为 7155927/11866247 B；这是有限轮次观察，无法单独归因于资源句柄。首次 Pai 上传时按文件名推导出错误测试 ID，部署返回 `staged_source -5`；使用正确 ID 完成安装，最后删除了错误 inbox 上传。Korvo 卸载命令客户端报设备暂未响应，随后查询确认测试包已卸载。两板最终原有包列表恢复，所有应用 inactive，未清理原有 staged 文件。

本阶段证明 Weather 的 IMAGE 路径和生命周期可用，但仍未满足整体“更小内存”的要求。后续应优先针对真实缩放 UI 图设计可验证的紧凑驻留格式或按可见区域虚拟化，不接受缺图、像素改变、每帧文件读取或在绘制热路径新增堆分配。arcade/common 的旧路径、完整 LVGL 原生预算与故障原子性、睡眠/唤醒矩阵、游戏 CPU/整帧 p50/p95 和最终逐项审计继续未完成。

随后 helper 的集合长度改为 `size_t`，防止 257 项声明绕回到 1 项并被误接受；新增超长表原生测试通过。最终 Weather 模拟器包已据此重建，`weather-image-final-comparison/report.json` 再次通过 5 个实包场景与 6 张逐字节截图。双架构设备包 `weather-image-hardware-final-packages/pxa-pxa-wimg.pxa` 也据此重建，AOT 哈希与前一版不同。Korvo 的最终 AOT 已安装、显示图标、记录 `weather: images ready`，退出回到 11866419 B 聚合 free_heap，随后测试包卸载；卸载命令客户端再次短暂失去 PACKAGE 响应，列表复查确认卸载成功。Pai 最终包上传到约 10% 时 USB Serial/JTAG 断开，PXADB 首次重启出现内部 RAM 不足，第二次完整复位后日志显示 PXADB 就绪、free_sram 27683 B。此时设备包列表已由外部操作变为 Store 0.8.1、已安装 Weather、staged Pixel Dungeon 等；未覆盖或删除这些内容，inbox 查询也没有本轮测试的残留文件。因此 Pai 的最终 AOT 没有完成实机重测，不能沿用此前旧 helper 布局的实测充当最终包证据。相关原始启动日志为 `weather-image-hardware/pai-final-boot-r3.log`，Korvo 最终截图和 PXADB 记录在 `weather-image-hardware/korvo-final/`。

## Weather 绘制成本、Pai 最终 AOT 与 PXADB 包身份修正

在相同签名旧/新 Weather AOT、固定 HTTP 响应及三处 296×240 滚动位置下，新加 `PXA_WEATHER_BENCH_FRAMES` 仅在测试 runner 中对稳定 UI 的 LVGL ARGB 快照进行 20 帧预热和逐帧计时。`weather-ui-performance/report.json` 保存 5 组交替顺序、每处每组 200 帧的原始 CSV、包/AOT/runner 哈希及逐屏像素 SHA；同组旧/新像素完全一致。三处 p50 绘制时间分别从 1.597/2.425/1.781 ms 降至 0.872/1.057/0.944 ms，降幅 45.4%/56.4%/47.0%；p95 降幅 49.4%/54.0%/49.0%。计时包含快照分配和 LVGL 软件绘制，不包含 Guest 业务、VSync、物理显示或 ESP CPU；它只说明当前桌面 UI 绘制性能，不抵消上节 22,570→127,086 B 的共享资源稳态回退。

固定 240×240、20 张 INDEX8 驻留且四张可见的 game-render 光栅基准补充了执行内核阶段计时，并重新构建保存基线与当前实现。`raster-current-15/report.json` 首组 15 轮整帧 p50 回退 6.9%；随后补充阶段计时的 `raster-stage-15/report.json` 为 p50 0%、p95 +4.3%，`raster-stage-30/report.json` 的 30 轮为 p50 +1.5%、p95 −0.3%。各组最终像素 checksum 一致。Ryzen 7 4800H 的动态频率和调度噪声显著，这些相互矛盾的结果不能当作稳定改进或稳定回退；整帧真实游戏、多轮受控及 ESP 测量仍需完成。补充计时本身改变了基准二进制，首组不能与后两组混为同一受控实验。

Pai 设备恢复就绪后，最终双 AOT 测试容器完整上传并安装为独立 ID `pxa-wimg`，四个网络 origin 分别授权，最终首屏截图 `weather-image-hardware/pai-final/weather-pai-final-ready.jpg` 显示天气、温度和图标。HOME 返回桌面、再启动同一测试 ID 后的 `weather-pai-final-resume.jpg` 仍有图标和数据；随后停止并卸载测试 ID，删除仅本轮上传的 inbox 容器，列表复查只保留原有 Pixel Dungeon、Store 0.8.1 和 Weather 0.3.2。起始/停止后的聚合 free_heap 为 7,217,239/7,148,719 B，卸载后为 7,155,327 B；期间网络权限和系统 UI 状态改变，不能用差值推导应用泄漏或内存优化。Pai 最终 AOT 的实机视觉与返回前台验证现已完成，但没有设备级 p50/p95 或 PSRAM 分类峰值。

这次安装暴露 `pxadb package install` 使用容器文件名 `pxa-pxa-wimg.pxa` 推断部署 ID 的错误：固件清单 ID 实际为 `pxa-wimg`，上传后 `staged_source` 返回 NOT_FOUND。已改为从有界 PXAC/PXAM 清单读取 ID，目录包也按清单读取；显式 `--identity` 与清单冲突时上传前报错。55 项 PXADB 回归和真实容器身份解析通过。此前失败上传造成的 staged 文件已清理，不修改其它应用。整体目标仍未完成：Weather 的更小内存方案、arcade/common 旧图片路径、完整 LVGL 原生内存/OOM、睡眠唤醒、真实游戏同画质多轮性能与最终逐项审计仍待完成。

进一步源码审计确认 `lv_draw_sw_img.c` 的缩放分支 `transform_and_recolor` 每次绘制都会调用 `lv_malloc` 创建临时变换缓冲并在结束时释放。Weather 当前所有 icon 节点使用 `IMAGE_FIT_CONTAIN`，通常源图 64×64 与节点尺寸不同；因此上面的绘制加速仍不满足“稳态绘制零堆分配”的目标。LVGL 的 `lv_draw_image.c` 明确拒绝对逐块解码图片做缩放，这也解释了前阶段 INDEX8 逐行尝试的裁切失败。下一步应优先验证加载前完成缩放且相同输出的尺寸专用驻留表示，或直接支持紧凑调色板资源的缩放绘制；必须同时复查完整内存、逐像素结果和绘制分配，不能只看快照时间。

## Guest SDK v1 默认入口与开发体验

应用户新要求，`sdk/guest-c/include/pxa.h` 已改为 Core v1 默认入口，公共状态码、事件和服务常量收敛到 `pxa_common.h`，v0 报文/导入保留在 `pxa_v0.h` 供旧辅助层及原生测试过渡。`pxa.h` 先于 `pxa_ui.h`、`pxa_canvas.h`、`pxa_raster.h` 或 `pxa_store_installer.h` 时自动选择 v1 传输和 64 位资源句柄；原有显式 v1 façade 仍可用。新增无堆 token 游标、生命周期事件形状解析、带默认 1.0 SDK 版本的 `hello-v1` 可打包示例和文档。v1 报文构建在 payload 已位于目标缓冲区时原位填写头部，省去逐字节自拷贝。

新包打包默认 SDK 1.0，拒绝显式 v0 Core，Wasm 链接许可不再包含 `pxa_control`。18 个内置应用重新打包成功，逐一检查其 19 个 Wasm Component 只导入 `pxa.core.v1`；示例的缺省清单 `minSdk`、`targetSdk` 实际均为 1.0，显式 v0 示例构建在编译前被拒绝。Guest SDK 原生回归与包工具回归通过。本轮未迁移 Host 对已安装 v0 包的兼容分支，也未把大型 header-only 辅助状态机贸然改成 `.c`；后者需用多翻译单元 Wasm/AOT 大小和构建时间对照证明收益。资源目标的剩余项仍按上节继续。

## Weather 固定图标预缩放与 LVGL 版本对齐

此前桌面模拟器固定 LVGL 9.5.0，而三板固件 lock 均为 9.6.0~1；同一张 64×64 图在两个版本的软件缩放结果并不相同（18×18 info 图离线像素有 130 字节差异）。桌面默认版本现对齐为 9.6.0，`tools/simulator.sh` 在本地存在 firmware managed source 时显式选择它，以免旧 build 目录缓存继续使用 9.5。适配器、系统 UI、renderer 和模拟器中的相关通用 flag 调用更新为 9.6 专用接口；复合 flag 保留一个局部 helper。桌面与原生测试配置从旧的 `LV_COLOR_DEPTH=32` 等价迁移为 `LV_COLOR_FORMAT_XRGB8888`。使用本地 firmware LVGL 9.6 源码、Debug、`PXSYS_WARNINGS_AS_ERRORS=ON`，`pxsys_desktop_simulator`、`pxsys_weather_images_test` 和 `pxa_lvgl_ui_test` 均构建通过，后者原生回归通过；独立 simulator CMake 开启完整 `PXSYS_BUILD_TESTS=ON` 会触发既有的 `pxsys_resources_test` 目标重名，故 UI 回归用独立 libpxa 构建验证。

Weather 只有 `info` 图标始终显示为 18×18。保留 64×64 原 PNG，新增由 LVGL 9.6 软件变换结果离线生成的 `info-18.png`，包中仍作为原 `assets/info.pxr` 使用。相同 Guest AOT（两包均 111232 B）、相同 296×240 画面下，64×64 PXR 基线与 18×18 候选的三处 BGRA 截图逐字节相同；正常、后台恢复、低预算、加载中退出共五个签名 AOT 场景通过。将两包复制到等长目录路径后，共享资源外部峰值 127096→112008 B（减少 15088 B），IMAGE 像素驻留峰值 115353→100265 B（减少 15088 B），元数据均为 11263 B；内存报告不包含 LVGL 原生堆。`weather-prescale-equal-path-compare/report.json` 保存包 manifest 哈希、运行命令、内存与六张截图比较，`weather-prescale-final-sources.json` 保存相关脏工作区源码哈希。与旧 PNG 路径的六张截图也完全相同，记录在 `weather-prescale-final-legacy-compare/report.json`；但相同首屏共享资源外部峰值仍是旧 PNG 路径 41531 B 对当前签名实包原路径下的 112002 B，尚不能宣称 Weather 总内存优化完成。两种路径的元数据记账不同，不能把这组旧 PNG 对比当作严格控制目录路径的总内存差值。本阶段没有新的 ESP 性能结论。

另外三张固定尺寸图标（location 19、clock/refresh 17）的预缩放试验可将相同场景共享峰值暂降至 66521 B，但三处截图中两处出现 26/71 个像素不一致，**没有进入正式资源**。`tools/audit-weather-palettes.py` 依据当前 `resources.json` 校验实际 PXR 与 PNG 字节相同：12 张图当前 BGRA 像素合计 181520 B，假设每张使用固定 256 色 RGBA 调色板加 INDEX8 可降至 57668 B；这只是离线无损可行性数字，LVGL 当前缩放路径仍不能直接消费它，不能当作已获得的运行内存收益。后续仍须完成可逐像素验证的紧凑图像驻留/绘制方案、LVGL 原生堆及热路径分配测量，并继续完整目标其余验收。

## Guest SDK 高层头文件的 v1 默认选择

复核发现先前四个高层头文件只有在 `pxa.h` 预先包含时才选 v1；独立翻译单元直接包含 `pxa_canvas.h` 等时会选 v0，造成 API 类型依赖包含顺序。现在 UI、Canvas、Raster、Store 高层头文件单独包含也默认 v1，只有原生旧协议测试显式定义 `PXA_GUEST_LEGACY_V0` 才进入 v0 分支。`pxa_test.c` 把四个高层头放在 `pxa.h` 前并验证 v1 传输与 64 位句柄。完整 Guest SDK/App 原生测试通过；18 个应用的 simulator AOT 和 Wasm 均重新打包成功，19 个 Wasm Component 的实际导入只含 `pxa.core.v1`（原始哈希在 `sdk-v1-default-wasm-all/imports-report.json`）。与上轮保存的同配置 Wasm 逐包比较，18 个应用大小全部完全相同；这个默认选择修复没有改变现有应用的编译产物体积。旧协议仍只为测试和迁移保留，不作为新应用入口。

三板固件随后按 Pai Touch → ESP32-S31 Korvo-1 → Sensecap Watcher 顺序构建通过；Pai/Korvo 使用 IDF main，Sensecap 使用 IDF 5.5.4。最终 app bin 分别为 3301056、3620224、3323712 B，三板 codec 强预算审计也通过。Sensecap 第一次链接时共享的 `PxaSystemSources.cmake` 在 CMake 生成构建图后更新，导致新 lock-screen 字体定义未编进库；重新配置后的第二次构建已包含该源并通过，不能把第一次失败当成通过。原始日志为 `weather-prescale-{pai,korvo,sensecap}-firmware-build*.log`。本阶段没有刷写这三份新固件。

ESP32-S31 Korvo-1 的旧固件（PXADB 版本 `e6bb9d9-dirty`）另安装并运行当前 Weather 的 `esp32s31` 签名 AOT，允许一次 `ipwho.is` 网络来源后，800×480 实机截图 `weather-prescale-korvo-grant1.png` 显示真实 San Jose 天气和缩小后的 info 图标；这证明新资源在现有设备 Host 上可加载和显示，不等同于逐像素桌面比较或新固件的设备运行验证。测试后停止、卸载 Weather，复查原有四个应用列表与测试前一致，inbox 无残留；Pai 当时有正在运行的 Garden Guard，本阶段没有打断它。

## Guest SDK v1 高层解析与头文件依赖清理

继续检查默认入口时发现 `pxa_ui.h` 虽然以 v1 发送事务，但同名 `pxa_ui_parse_event`、pointer/controller/environment/theme/pressure/surface 解析函数仍以 v0 `pxa_event_t` 为参数，`pxa_ui_theme_get` 的请求 token 只有 32 位。现在这些高层接口随默认传输采用 `pxa_event_t` 与 64 位主题 token；原 `pxa_ui_v1_parse_*` 名称委托同一实现，避免两份解析逻辑长期分叉。旧协议测试显式选 v0 时仍保留旧签名。

通用字节 builder/read 函数从 `pxa_v0.h` 抽到 `pxa_writer.h`，共享 DrawList 写入常量与编码函数抽到 `pxa_game_render_wire.h`。默认 UI/Canvas/Raster/Store 四个高层头文件单独包含时均不再传递包含 v0 导入声明，测试中加了编译期检查。`PXA_CLOCK_TICK` 等共享 opcode 放到 `pxa_common.h`，避免原生测试依赖 v0 的间接包含。完整 Guest SDK/App 原生测试和新增 64 位主题 token、输入、压力、surface 错误事件用例通过；三类代表应用 arcade、voxel-craft、Store 的新 Wasm 包构建并通过导入审计，均只导入 `pxa.core.v1`。与上轮保存的同配置 Wasm 比较，三者字节数分别为 244335→244449、171934→171982、157457→157478；本改动没有可宣称的 Wasm 体积下降，实际增量为 114/48/21 B。未改变 Host 或 ESP 固件，本阶段没有新增设备性能结论。

三类应用的 simulator AOT 也重新打包成功；AOT 字节数分别为 arcade 300996→301108、voxel-craft 274508→274508、Store 249284→249284。包工具回归最初暴露 standalone arcade 旧协议试验编译没有显式旧协议选项，补齐 `PXA_GUEST_LEGACY_V0=1` 后全套通过。该旧协议开关只用于测试，不进入正式应用包。

SDK 的 v0 服务头和 Host 对已安装旧包的兼容路径仍存在；“v1 已成为新包默认且默认四个高层头不会引入 v0”不等于 v0 实现已经全部删除。头文件拆出的是跨传输共享的小函数，未新增 Guest 链接库；是否把较大的状态机改为 `.c` 还需要同一应用多翻译单元的 Wasm/AOT 体积、构建时间与热路径对比。资源总目标其余验收继续有效。

## 默认 Guest 服务头完整转向 v1 与 Weather 预乘图像校正

默认 Guest 服务头进一步覆盖 Audio、Device、FS、GameRender、IPC、Lease、Log、Net、Permission、Sensor、Storage、Surface、System、Work；这些头和原先的 UI、Canvas、Raster、Store 独立包含时均不再带入 v0 传输。旧原生测试显式定义 `PXA_GUEST_LEGACY_V0`；新测试一次包含全部默认头并在编译期拒绝 v0。System 配置记录解析抽为共享头，避免 i18n 的 v0/v1 解析分叉；默认 i18n 事件参数为 `pxa_event_t`。设备 MAC 文本格式化、格式位常量和日志级别抽成纯共享头；Store 与 game-render-bench 不再为了这些常量包含旧服务头。Guest SDK/App 回归通过；正式包仍需在最终变更集上重新验证。当前做法不新增 Guest `.c` 或链接库，旧 v0 代码仍为独立兼容测试保留。

此前“info-18 与旧版三张截图完全一致”的表述存在覆盖盲区：这三张截图均未实际显示 info 图标。增加七个滚动位置后，原先直通 alpha 的 18×18 资源在显示 info 的屏幕出现 26 个像素差异，因此不能据此认定原预缩放方案保真。原因是 LVGL 9.6 的缩放分支输出预乘 alpha，而原资源仍被声明为直通 BGRA。PXR IMAGE 新增 encoding=8 预乘 BGRA8888，Host/LVGL 直接按预乘格式解释。四张固定尺寸图标在构建前由相同 LVGL 9.6 变换生成原始像素，资源编译器验证长度及颜色通道不大于 alpha；原始 64×64 PNG 仍保留为可再生源。`weather-four-premul-comparison/report.json` 的等长路径签名 AOT 对比覆盖十个滚动位置、五种场景和二十组截图，像素全部逐字节一致；背景恢复、低预算、加载中退出后的资源归零也通过。共享外部峰值 127098→66614 B，IMAGE 像素峰值 115353→54869 B，元数据均为 11265 B，Guest 线性峰值均为 102400 B；这些数值不包括 LVGL 原生堆，且仍高于旧 PNG 路径的共享外部峰值 41531 B，不能宣称 Weather 总 RAM 已下降。`weather-four-premul-performance/report.json` 的十屏交替抽样 p50/p95 有升有降，未证明稳定的整体帧速提升。调色板离线审计脚本现能读取预乘 BGRA 源及对应签名 PXR；当前十二张实际资源像素总计 136124 B，固定 256 色假设为 46319 B，但 LVGL 仍不能直接缩放这种假设编码，不能计入内存收益。`pxa_asset_catalog_test`、`pxa_lvgl_ui_test` 和资源编译器十项测试通过。

最终源码的 Store、game-render-bench、Weather simulator AOT/Wasm 包重新构建成功；三者实际 Wasm 导入均通过 Core v1 审计。Store 的 AOT 为 249284 B，与上轮同配置包相同；Weather 的 simulator AOT 与上述逐像素对比包 SHA 相同，四张预乘 PXR 与 catalog 字节也相同。包清单 SHA 不同是因为最终包额外包含 Wasm 回退，不能把它标成同一个签名容器。默认头测试、Guest SDK/App 全套测试、包工具测试和 24 项 Core 测试通过。Pai Touch、ESP32-S31 Korvo-1、Sensecap Watcher 在各自原有工具链配置下顺序增量构建成功；强 codec 预算审计分别覆盖 85、84、85 个对象且四个入口均通过，构建日志见 `build/resource-refactor/pxa-weather-premul-{pai,korvo,sensecap}-build.log`。这只证明编译与静态预算，设备运行结论另记。

Korvo `/dev/ttyUSB0`、2000000 baud 上进一步实测新固件：起始固件 `e6bb9d9-dirty`、无活动应用，先读取 0x10000 起的完整 4 MiB 应用分区备份（SHA-256 `d9fe9f546d4f75ee7bbc52096929aa2ab3578c4f888245f84c3a7e9e68c36fc2`），再仅刷写本轮 ESP32-S31 app bin（SHA-256 `5a0e837f5ce3804f8e89f3181067e8972e98348bc0d4f97375c8c131b32ddc01`）。启动报告新固件 `202d179-dirty`，Weather 的 S31 签名 AOT 容器安装并运行；允许所需网络 origin 后，800×480 面板截图 `weather-four-premul-korvo-weather.jpg` 显示位置、当前天气和缩小的 info 图标，`weather-four-premul-korvo-scroll.jpg` 显示预报/历史列表。返回桌面并再次进入后的 `weather-four-premul-korvo-resume.jpg` 仍显示天气、位置和图标。实测证明新 encoding=8 资源可被 ESP Host 加载及 LVGL 显示；截图不能证明与旧固件逐像素相同，也没有可靠的设备帧时间、native LVGL 内存或 FPS 数据。测试包停止、卸载后应用列表恢复为原有四项，`pxa-state` 下无 inbox 残留。最后完整写回备份应用分区并通过 esptool 写入校验，设备报告恢复 `e6bb9d9-dirty` 和原四项应用。备份与刷写日志为 `build/resource-refactor/weather-four-premul-korvo-{backup,flash,install,restore}.log.gz`，原分区备份为同目录 `weather-four-premul-korvo-before-app.bin`；未修改 Pai 当前运行的应用。

## LVGL 图片阶段验收收束

Weather 只保留上述四张固定尺寸预乘图标，原始八种天气图仍保持 64×64。曾尝试为天气图另外生成 24/28/68 像素变体及 25 像素标题变体：十个滚动位置的正常与后台恢复截图均与基线逐字节一致，但共享外部峰值在小变体试验中由 66616 增至 67243 B，加入 68 像素变体后首屏由 66616 增至 71825 B、IMAGE 驻留由 54981 增至 59060 B。变体虽减少部分绘制缩放，却增加元数据和驻留，故全部撤回，不纳入最终资源。试验原始结果保留在 `build/resource-refactor/weather-compact-icons-comparison/` 与 `weather-compact-all-comparison/`，不作为通过的性能结论。

撤回后 `resources.json` 恢复 12 项，四张预乘源经 LVGL 9.6 生成器 `--check` 逐字节复核；当前源码重编的 12 个 PXR 与上述已签名验收包 `weather-four-premul-candidate/pxa-weather/assets/` 全部逐字节相同。资源编译器十项测试和根工程、System 子仓库的 `git diff --check` 通过。四图方案的二十组逐像素比较、生命周期/预算场景、三板构建及 Korvo 实机显示仍是本阶段验收依据。LVGL 图片专项到此结束；未覆盖的原生 LVGL 堆、整应用内存与 ESP 帧时间不推断为已优化，资源总目标的其他验收项仍按目标文档执行。

## Voxel Craft 真实玩法场景的资源与光栅测量

`pxsys_voxel_resources_test` 在原菜单截取后可选注入 NEW GAME 指针事件，真实签名 AOT 随即进入玩法并保存连续 60 个 240×320 RGB565 帧、每帧 Host 光栅时间和内存快照。旧源码使用最初保存的 `baseline/voxel-source/`，原签名包的 icon/Ogg 文件补回到独立构建副本；当前源码另存快照。两包均用相同 `VOXEL_FIXED_SEED=1234,VOXEL_FIXED_VIEW=1` 编译，Release runner、发布公钥、包 manifest/AOT 与源码文件摘要记录在 `voxel-gameplay-cpu-comparison/report.json`。本测试不改变正式应用编译选项。`voxel-gameplay-fixed-resources/report.json` 再次证明 48 张纹理、4096 个调色板色和完整菜单逐字节一致。

五轮按旧→新、新→旧交替运行；每轮 60 帧中，按完整像素内容可跨包精确配对 41、41、57、57、57 帧。其余帧受游戏时间、HUD FPS 和事件调度影响，不能称为全部游戏帧逐像素一致。仅对匹配帧统计的 Host 光栅时间，各轮 p50 有升有降；五轮 p50 中位数 1076→1029 μs、p95 中位数 1226→1186 μs。测试入口又以编译期开关测量 Guest tick、Surface 执行与下一次 LVGL timer 处理的 CPU 路径，排除休眠；此路径的 p50 中位数 4584→4446 μs，p95 为 4943→5049 μs，各轮同样有升有降。**这些数据不足以宣称稳定提速或完整帧时间达标**：CPU 路径未涵盖 tick 外的事件处理、异步 worker、设备呈现及真实 ESP CPU，也不是端到端延迟。原始 60 帧与逐帧 CSV 均保留。

相同玩法画面下，AOT buffer 374748→274556 B，Guest 线性内存峰值 1167360→1015808 B，二者分别减少 100192/151552 B；绑定的资源对象分配均为 47444 B。共享预算同时峰值的内部部分 8240→0 B，外部部分 41800→61419 B，后者**增加 19619 B**。worker 元数据约 1444→12823 B，两包配置栈均为 131072 B。上述类别会重叠或遗漏 LVGL/codec/libc 私有分配，不能直接相加推出总 RAM 净节省；当前玩法的资源管理确实增加 Host 外部预算与元数据。相关脚本为 `tools/compare-voxel-gameplay.py`，runner 仍支持原四参数菜单测试。完整应用内存时间线、同输出整帧 p50/p95、ESP 实机性能与原目标剩余验收继续开放。

在工作区根目录可用已签名的两份固定视角包重跑：`cmake --build build/simulator/product-lvgl96 --target pxsys_voxel_resources_test -j4`，随后执行 `python3 tools/compare-voxel-gameplay.py build/resource-refactor/voxel-gameplay-fixed-old/pxa-voxel-source build/resource-refactor/voxel-gameplay-fixed-new/pxa-voxel-craft local/pxa-apps/.dev-signing/publisher-public.pem --runs 5 --output build/resource-refactor/voxel-gameplay-cpu-comparison`。构建用的旧/新完整源码快照位于 `build/resource-refactor/voxel-gameplay-fixed-sources/`，两包均用 `PXA_APP_DEFINES=VOXEL_FIXED_VIEW=1,VOXEL_FIXED_SEED=1234` 及当前 package tool 生成；旧快照中的 icon/Ogg 与最初签名基线包相同。两包采用同一 Release runner，报告保存文件身份与全部逐帧原始数据。

补充十轮绑核交替对照，命令同上但增加 `--runs 10 --cpu 0 --output build/resource-refactor/voxel-gameplay-pinned-comparison`。每轮仍取 60 帧，旧/新精确匹配 41～57 帧。匹配帧的十轮 p50 中位数：Host 光栅 911.25→1029.5 μs，CPU 路径 3437→3498.75 μs；p95 分别 1212.5→1255 μs、4732→4716.5 μs。光栅 p50 在这一组测量中回退约 13%，CPU 路径 p50 回退约 1.8%，各轮方向并不一致；结合前述五轮结果，**不能宣称稳定提速，也不能据此断定真实设备上的稳定回退幅度**。另一组五轮非绑核 `voxel-gameplay-memory-comparison/report.json` 中 glibc `mallinfo2` 的 `uordblks` 为旧 940128 B、新 959232～959264 B，`hblkhd` 为旧 19939328 B、新 19841024 B；两类数字与 WAMR、共享预算等相互交叠且只属于桌面进程，不能推导 ESP 内存净值。两组原始逐帧数据、源码/包/runner 摘要均保留在对应报告目录。

Pai Touch `/dev/ttyACM0`、固件 `202d179-dirty` 上，当前普通 ESP32-S3 签名 Voxel 0.2.0 包安装、授权音频、进入 NEW GAME 后正常显示地形、物品栏和 HUD；`voxel-device-pai/gameplay.jpg` 等实机截图记录可见玩法。当前固定种子/视角且自动进入游戏的 ESP32-S3 包也能运行，`fixed-new-gameplay-{4,5,6}.jpg` 的连续合成帧计数为 212、463、564，对应设备时间为 3581069613、3594846452、3600296873 μs，两个区间约为 18.2 和 18.5 次显示提交/秒，HUD 显示约 18 GFPS。这是单板短时观察，不是 CPU p50/p95 或长期稳定 FPS。旧源码构建的同参数 ESP32-S3 签名包可安装并启动，授权、解锁及桌面重进后均未取得游戏帧，截图仍为 LVGL 黑屏，故**没有可信的实机旧/新 FPS 对照**；原因未确定，不能将它归因于资源重构或 ABI。期间设备会进入锁屏，帧计数仅使用确认显示玩法的合成截图。测试结束后临时 Voxel 包已停止并卸载，Pai 应用列表复核恢复测试前的 Weather 0.3.2、Store 0.8.1、Jump Jump 3D 0.1.3、Maze Evil 0.1.3，均 inactive；未改动 Korvo 的安装内容。
Korvo `/dev/ttyUSB0`、2000000 baud 上运行了设备原已安装的 Voxel 0.2.0 包，800×480 菜单和 NEW GAME 后的游戏画面均正常，截图为 `voxel-device-korvo-{menu,gameplay-1,gameplay-2}.jpg`；两张玩法图的 HUD 约为 25 GFPS。该机固件是此前恢复的 `e6bb9d9-dirty`，原安装包的源码摘要未知，PXADB 对 panel-framebuffer 截图返回的 `frame_id=0` 也不能用于帧率计算，因此这只证明原有 Korvo 路径可运行，**不构成当前源码新旧版本的受控性能比较**。测试后仅停止该应用，原四项应用仍全部安装且 inactive。

## Voxel 加载与玩法内存时间线

`pxsys_voxel_resources_test` 新增独立 `timeline` 模式：首次产品事件循环起，加载期至多每 10 ms 采样，随后记录菜单和 60 个玩法帧的 WAMR、Guest 线性内存、共享预算同时驻留、资源缓存、worker 元数据及 glibc `mallinfo2`。它与原 `gameplay` 性能模式分开，逐帧采样开销不进入上一节 p50/p95。重跑命令：

```sh
cmake --build build/simulator/product-lvgl96 --target pxsys_voxel_resources_test -j4
python3 tools/compare-voxel-gameplay.py \
  build/resource-refactor/voxel-gameplay-fixed-old/pxa-voxel-source \
  build/resource-refactor/voxel-gameplay-fixed-new/pxa-voxel-craft \
  local/pxa-apps/.dev-signing/publisher-public.pem \
  --timeline --runs 1 --cpu 0 \
  --output build/resource-refactor/voxel-gameplay-timeline-comparison
```

报告保存 runner/签名 AOT/清单/源码摘要、逐帧 CSV 与原始日志。菜单逐像素相同，60 个玩法帧中本轮 41 帧可按完整 RGB565 内容精确配对。玩法期间旧/新版的 Guest 线性内存始终分别为 1167360/1015808 B，AOT buffer 为 374748/274556 B；共享预算内部同时驻留为 8240/0 B、外部为 41800/61419 B，后者包含新版 47444 B 的文件纹理/调色板缓存，**不能把它再与共享外部相加**。worker 元数据 1444/12823 B。菜单时桌面 `mallinfo2().uordblks` 为 810800/946656 B，第 60 帧为 940128/959264 B；`hblkhd` 第 60 帧为 19939328/19841024 B。这些计数包含相互交叠的 Host/WAMR/LVGL/libc 分配，仍不能推导 ESP 内部 RAM 与 PSRAM 的净变化，也不是完整 native 分类总峰值。

旧版同步纹理上传发生在首个产品事件循环之前，因此加载期只有一个可见采样点；新版异步加载在本轮留下 11 个采样点。10 ms 间隔可能漏掉瞬时峰值，各类别最大值也可能不在同一时刻，不能相加称作总峰值。该工具推进了实际应用的同时间轴取样，但完整目标仍需把 LVGL、codec、任务栈、显示缓冲及 ESP 分区内存纳入可核对的分类报告，并补睡眠/唤醒与最终性能验收。

## ESP 按需内存诊断与 Pai Touch 玩法取样

PXADB 新增 `memory` 命令，显式请求时才读取 ESP heap capabilities 的 SRAM/PSRAM 容量、空闲、历史最低空闲和最大空闲块，以及 PXA 共享预算的当前/峰值、临时空间峰值、六类当前占用、拒绝与底层分配失败次数。数据通过现有 Host 预算锁取得，不在渲染、音频回调或每帧执行。`pxadb memory --port /dev/ttyACM0` 返回 JSON；Korvo 使用 `--port /dev/ttyUSB0 --baud 2000000`。预算类别是堆占用的子集，不能与 heap used 相加；WAMR、LVGL、codec、任务栈、显示驱动等未全部走共享预算，仍需单列。

PXADB 主机 56 项回归通过，Pai Touch、ESP32-S31 Korvo-1 和 Sensecap Watcher 当前源码固件顺序构建通过，最终日志为 `pxadb-memory-{pai,korvo,sensecap}-final-build.log`。第一次 Pai 构建暴露生成的锁屏 LVGL 字体使用 `lvgl/lvgl.h`，而当前组件只导出 `lvgl.h`；给 reference UI 编译目标设置 `LV_LVGL_H_INCLUDE_SIMPLE=1` 后重建通过。三板最终链接产物包含新的 PXADB 与 Host 查询接口。设备运行只在 Pai 上验证，Korvo 和 Sensecap 未刷写新固件。

Pai `/dev/ttyACM0` 的原应用区域先备份 0x10000 起 4 MiB，SHA-256 为 `eff19db6f5fef9576230e02d15fc68671729c68dff9bde14b5d2d0da96de4f6c`；新 app bin 为 3311648 B，SHA-256 为 `6d28e892cc29b27e44c4e90441980c1b90a8bf2f779a34642bd63ec515b4ca3f`，完全落在备份范围内。esptool flasher stub 的两次整段读取和一次 64 KiB 读取均在地址 0x157000 中断；改用 `--no-stub` 的 ROM 读取成功跨过该地址并完成备份，因此未把失败读取当作有效备份。随后仅刷 app 镜像；新固件 HELLO 宣告 `memory`，命令实机返回所有范围和类别。

签名 Voxel 包安装后、应用未运行时，SRAM/PSRAM 空闲分别为 45267/7206868 B，共享预算内部/外部为 27468/16516 B。运行当前固定种子/视角自动玩法包，`pxadb-memory-pai-gameplay-confirmed.jpg` 显示实际地形、物品栏和约 16 GFPS；可见玩法附近的快照空闲 SRAM/PSRAM 为 35919/4553916 B，共享预算内部/外部为 28940/74348 B，其中外部纹理 47444 B、音频 16392 B、元数据 10512 B；内部 28940 B 均属当前预算元数据。与安装后空闲状态相比，堆空闲减少约 9348 B SRAM、2652952 B PSRAM，但还包含 AOT、Guest 线性内存、系统 UI 和其它运行期开销，**不能把差值全部归因于资源**。期间有锁屏，故只对已确认是合成玩法画面的截图关联可见状态；`pxadb-memory-pai-autoplay-log.txt` 另记录实际玩法的 Guest/Host 渲染日志。应用停止后共享预算精确回到内部/外部 27468/16516 B、纹理类别归零；系统 PSRAM 空闲未立即回到安装前数值，不能仅据此判断泄漏或完全回收，需要同条件多轮 native 生命周期审计。

最终源码顺序构建后的 Pai app bin SHA-256 为 `70925bad890d74e7e379717639ceb9565502a0d5b973a1432a097a4d10b0e2e7`，与上面的初次实测镜像不同，故又将这一**确切最终镜像**单独刷入 Pai 复验。`pxadb-memory-pai-final-second.jpg` 确认固定场景的实际玩法与约 18 GFPS；同次 `memory` 的共享预算内部/外部仍为 28940/74348 B，其中纹理 47444 B，系统空闲 SRAM/PSRAM 为 35931/约 455 万 B。最终镜像的三份 JSON 分别记录未安装测试包的空闲、玩法和停止后的状态；第一份在安装前，不能拿它与玩法直接当成纯应用内存差值。停止后资源预算回到 27468/16516 B，纹理归零。

两轮测试包均已卸载，Pai 包列表与测试前逐项一致。原 4 MiB 应用区域两次用 `--no-stub` 写回，esptool 均报 `Hash of data verified`；最终重启后 HELLO 不再含 `memory`，原固件和全部原应用恢复且均 inactive。`build/resource-refactor/pxadb-memory-report.json` 汇总备份/固件/截图和包列表摘要、两次实测的原始 JSON 及限制；备份、失败探测、刷写、恢复日志与 JSON 快照均在同目录 `pxadb-memory-pai-*`。这批数据首次给出 ESP 同时刻的 SRAM/PSRAM 与资源预算分类，但不是旧/新固件同画质对照，也未覆盖整个 Host 原生内存；总目标仍继续。

## v1 默认头回归与 Korvo 最终资源路径复验

`tools/test-pxa-resources.sh` 完整回归重新运行时发现两处切换默认 Guest SDK 后的编译漏点：验证 v0 导入的 WAMR 测试仍包含 `pxa.h`，已改为显式 `pxa_v0.h`；v1 资源场景缺少 scratch 模式公开常量，Pixel Dungeon 仍用 v0 头中的坐标映射函数。v1 game-render 头现公开三种 scratch 常量和同语义的坐标映射辅助函数，两个真实应用使用 v1 名称，v1 单元测试覆盖映射边界。修复后完整一键回归通过，包括 Core/WAMR ASan/UBSan、ESP Host、规范/打包、Store/Plane/Pixel 实际包、LVGL、低预算、慢存储及退出场景；Guest SDK/App 全套测试也通过。原始日志在 `build/resource-refactor/final-resource-check-driver.log`、`final-resource-check/logs/` 和 `final-resource-check/guest-sdk-test.log`。这修复的是 SDK/示例编译契约，不改变 Host 传输或固件 ABI。

Korvo `/dev/ttyUSB0`、2000000 baud 起始原固件为 `e6bb9d9-dirty`，四个原有应用均 inactive。先备份 0x10000 起完整 4 MiB 应用分区，SHA-256 为 `d9fe9f546d4f75ee7bbc52096929aa2ab3578c4f888245f84c3a7e9e68c36fc2`，与此前恢复保存的原镜像逐字节一致；再刷入本轮构建的 app bin（SHA-256 `f8ae9113d64abada8610cf831fcb33fe4852e874c2ba7f3d387e8bf628e613e4`）。设备报告 `202d179-dirty` 且 HELLO 包含 `memory`；按需查询返回分类预算与 heap capabilities，Plane Shooter 原安装包的菜单文件图片在 800×480 面板显示正常，截图为 `final-resource-check/korvo-plane.jpg`。

为验证当前 v1 Guest 包，另将 `resource-scenes` 编译为 ESP32-S31 签名 AOT 并安装。首次启动被必需的 `audio.playback` 权限策略拒绝，串口记录 `authorize-required-permissions status=-4`；通过设备设置授予 `media` 权限后重新运行，`tools/verify-device-resources.py --rounds 1` 的五个终态标记全部出现。100 次场景切换约 39.996 秒，20 张文件纹理每场景只绑定 4 张，8193 B 地图分块读取、100 次音效触发以及音乐 READY/STOPPED 都完成；音频累计 `underruns=0 recovered=0 missing=0`。运行中同一时刻的共享预算内部/外部为 27556/485028 B，其中 raster 外部 459032 B；历史峰值内部/外部 29036/535362 B。停止后预算回到 27004/16564 B，raster 归零。原始串口、设备内存 JSON 和报告在 `final-resource-check/korvo-resource-verified/`、`korvo-memory-scenes-active.json`、`korvo-memory-stopped.json`。峰值是共享预算计数，不等于完整 ESP 堆峰值；本轮没有同画质旧版 Korvo 帧时间对照。

测试包已卸载，四个原有应用及 inactive 状态与开始时一致。随后将备份的 4 MiB 原应用分区完整写回，esptool 报 `Hash of data verified`，重启后的 HELLO 恢复 `e6bb9d9-dirty` 且无 `memory`，原应用列表再次确认一致。备份、刷写、安装、测试、恢复日志都保存在 `build/resource-refactor/final-resource-check/`。本轮实测补齐 Korvo 的新内存诊断与当前 v1 资源场景路径，但不代表完整目标已经完成；完整原生内存核算、熄屏/睡眠矩阵与可对照的真实游戏整帧 p50/p95 仍按目标文档保留。

## Korvo 锁屏生命周期修复与音乐暂停实测

源码核对发现 Pai Touch 和 Sensecap Watcher 均注册 Reference UI 的锁屏状态回调，并将锁屏传给 PXA Surface 与 Host；Korvo 的 `AttachSystem` 只保存 UI 指针，缺少这一回调。Korvo 因此无法在锁屏时通过 Host 的 `display_interactive` 门控暂停 UI Clock 和音频。现在 Korvo 使用与另外两板相同的锁屏通知路径；Host 只在生命周期状态切换时记录 `requested`、`display_interactive` 和最终前台状态，未在帧/音频回调增加工作。`tools/verify-device-lifecycle.py` 在一条 PXADB 连接中订阅状态日志、等待真实空闲锁屏、保持锁屏、解锁并核对应用停止，可重复运行：

```sh
python3 tools/verify-device-lifecycle.py \
  --port /dev/ttyUSB0 --baud 2000000 --app pxa-voxel-craft \
  --unlock-swipe 400 410 400 90 --timeout 180 \
  --output build/resource-refactor/korvo-lock-retest
```

本轮新 Korvo 固件 app bin 的 SHA-256 为 `efe507ebc250f2bb032e75b017a7fafb8cfc9a0d7d0c6bb234d6c01ecf208976`。设备原 0x10000 起 4 MiB 分区先完整备份，SHA-256 `d9fe9f546d4f75ee7bbc52096929aa2ab3578c4f888245f84c3a7e9e68c36fc2`，与上一轮保存的原镜像相同。运行设备原安装的 Voxel 包，实机串口记录在 1.19 秒进入前台、110.53 秒因 `display_interactive=0` 转入后台、121.66 秒解锁后重新进入前台、125.27 秒停止。锁屏保持期间两张 800×480 JPEG 截图 SHA-256 完全相同，解锁后截图恢复 Voxel 菜单。音乐从约 1.32 秒开始，停止时累计输出 1,804,160 个 16 kHz 样本，折合约 112.76 秒，接近音乐启动后两段前台时长之和；锁屏约 11.13 秒没有继续消耗，`underruns=recovered=missing=0`。这些是同次设备日志和显示取样，不是单看黑屏推断暂停。原始结果为 `build/resource-refactor/korvo-lock-retest/report.json`、`serial.jsonl`、`locked.jpg`、`locked-second.jpg`、`unlocked.jpg`。

原固件上也尝试了同应用对照，但一次运行中的频繁截图、另一次约 165 秒的无截图等待均未取得可用的锁屏样本；原始失败记录保留为 `korvo-lock-baseline-*`，**不能用它声称旧固件音频实际多消耗了多少**。新固件的锁屏/解锁和音乐暂停结论来自上述可复现的当前固件实测；旧代码缺失回调则由源码确认。ESP Host sanitizer 测试与 Korvo、Pai Touch、Sensecap Watcher 三板顺序固件构建通过，分别见 `korvo-lock-host-test.log`、`korvo-lock-lifecycle-final-build.log`、`korvo-lock-pai-build.log`、`korvo-lock-sensecap-build.log`。本节补的是 Korvo 锁屏路径；Pai/Sensecap 的物理熄屏、长时间睡眠和断电恢复矩阵仍未全部验收。

测试结束后已将原 4 MiB 应用分区完整写回，esptool 报 `Hash of data verified`；重新连接的 HELLO 为 `e6bb9d9-dirty`，不含测试固件新增的 `memory` 能力。原有 Plane Shooter、Store、Voxel Craft、Maze Evil 四个应用逐项确认仍为 `active=0`。恢复日志及设备查询结果为 `build/resource-refactor/korvo-lock-retest-restore.log`、`korvo-lock-doctor-restored.txt`、`korvo-lock-packages-restored.txt`。

锁屏修复后的完整 `tools/test-pxa-resources.sh` 再次通过，覆盖 Core/WAMR ASan/UBSan、ESP Host sanitizer、规范/打包、真实包、LVGL、低预算、慢共享存储和退出场景；原始驱动日志和逐项日志位于 `build/resource-refactor/korvo-lock-final-check-driver.log` 与 `korvo-lock-final-check/logs/`。根工程与 `deps/pxa-system` 的 `git diff --check` 也通过。

Pai Touch `/dev/ttyACM0` 上使用同一固件源码（app bin SHA-256 `4ef69f1ce53082c09082b08acad37a45712a74f1fb7eb764023f4c438d40d789`）及设备原安装的 Weather 包实测屏幕生命周期。刷写前完整读取 0x10000 起 4 MiB 应用分区，大小 4194304 B、SHA-256 `eff19db6f5fef9576230e02d15fc68671729c68dff9bde14b5d2d0da96de4f6c`，与之前原镜像备份一致。刷写后 HELLO 含 `memory` 能力。Pai 的空闲关闭屏幕需要先点按唤醒，再从屏幕底部向上滑动解锁；测试器新增可选 `--wake-tap`、解锁时长和步数参数，最终可重复命令为：

```sh
python3 tools/verify-device-lifecycle.py \
  --port /dev/ttyACM0 --baud 115200 --app pxa-weather \
  --wake-tap 148 200 --unlock-swipe 148 230 148 20 \
  --unlock-duration-ms 600 --unlock-steps 12 --timeout 210 \
  --output build/resource-refactor/pai-lock-weather-wake-verified
```

最终运行中 Bridge 于 1.36 秒确认 Weather 前台；Host 于 107.44 秒因 `display_interactive=0` 切到后台，118.83 秒唤醒解锁后切回前台，122.01 秒停止。两张锁屏 JPEG 哈希相同，解锁后的截图显示 Weather 页面；原始时间线与截图在 `pai-lock-weather-wake-verified/`。此前单次短滑动以及未先唤醒屏幕的两轮测试没有取得解锁转移，另有一次因前台转换日志未出现而未满足旧测试器的严格起始条件，保留失败报告，不能把它们算作通过。最终脚本要求 Bridge 确认启动身份以及 Host 真实 1→0 锁屏、0→1 解锁转换。测试后写回原 4 MiB 镜像，esptool 报 `Hash of data verified`；HELLO 不再含 `memory`，恢复前后的四个原应用清单逐字节一致，全部 `active=0`。备份、刷写、恢复与恢复后查询记录在 `build/resource-refactor/pai-lock-*`。本次覆盖 Pai 的自动熄屏/唤醒，不代表长时间睡眠或断电恢复已经验收。

## Voxel 固定视角旧包的实机复查与扩大主机测量

此前 Pai 上旧版固定视角包的黑屏样本不足以证明旧包无法运行。Pai 空闲后可能先关闭屏幕，注入手势需要先点按唤醒再完整上滑；本轮在原固件 `202d179-dirty` 上按此步骤运行同一份已签名旧包，`old-diagnose/{5,15,30,45}.jpg` 全部显示 296×240 游戏画面，日志有 `voxel: raster surface ready`、连续 GameRender 计数与性能采样。随后以同一固件安装新版固定视角签名包，`new-diagnose/{5,15,30,45}.jpg` 也全部显示玩法。两个包各运行约 45 秒并停止；测试 ID 最终卸载，设备原有四个应用清单与测试前逐字节一致。卸载时 PXADB 客户端没有收到响应，但随后包列表确认测试包已不存在；不能把客户端错误误判为设备仍安装。原始串口在 `voxel-device-pai/{old,new}-diagnose/serial.jsonl`。

这两个实机画面均为固定视角、相同分辨率和质量级别，但玩家以外的世界状态及自适应视距仍会随时间变化：两包 45 秒 JPEG 的天空/HUD 相近，下半画面存在差异，Host 候选四边形数量也不同。旧版在 15～45 秒的 13 次采样 GFPS 中位数 17.9，新版 14 次为 18.2；它们不是逐帧配对、不是 p50/p95，也不是同输出性能增益。旧包可正常显示这一结论推翻了前述“旧包无法取得游戏帧”的设备观察，旧黑屏失败记录仍保留以便追查锁屏与注入时序。

同一已签名 240×320 旧/新包和相同 Release runner，又在 CPU 8 上完成两组各 20 轮交替主机测量，每轮 60 帧，仅按完整 RGB565 匹配的帧参与对比。第一组匹配 15～57 帧，Host 光栅 p50/p95 的跨轮中位数为 1026/1121→1026.75/1126.5 μs，CPU 帧路径为 4353/4705→4360.5/4733.5 μs；第二组匹配 31～57 帧，对应为 1024/1142.5→1025/1107.5 μs 和 4352/4754.5→4365.5/4667.5 μs。两组在匹配帧上均未复现超过约 5% 的稳定回退，也未证明稳定加速；先前十轮光栅 p50 约 13% 的失败结果仍保留，说明主机频率/调度和动态场景造成噪声。原始像素、逐帧 CSV、包/runner/源码摘要和 20 轮报告在 `voxel-gameplay-pinned-20-{a,b}/`。该 CPU 路径不含完整设备呈现/VSync；实机还需冻结动态场景、采集逐帧时长并核对可见输出。

## Voxel 同输出 Pai 实机逐帧测量

为排除自适应视距、怪物、粒子、水动画及 HUD GFPS 数字的差异，旧版与新版源码快照都以相同的 `VOXEL_FROZEN_BENCHMARK=1` 测试开关冻结这些动态项，同时设置固定视角、种子 1234 和自动进入游戏。测试副本在 `build/resource-refactor/voxel-frozen-sources/`，不改发布版的游戏逻辑。模拟器先验证两份包的 3 轮完整 RGB565 精确配对分别为 60/59/59 帧，原始结果在 `voxel-frozen-final-sim-comparison/`。实机包分别为 `voxel-frozen-final-esp/pxa-voxel-{source,current}.pxa`，SHA-256 是 `285d907c8a23cee2136d6e54e79d1183b561416a74341326fe81edd324825e52` 与 `3c42ce6706877f8dbb372917119a939fc20cc0a838e14b3dee94615c105850fc`；两个包均未启用曾导致串口日志排队溢出的 Guest 性能打印开关。

Host 增加显式开启的 256 帧有界测量缓冲，启动时才占用约 2.1 KiB PSRAM，热路径只写逐帧整数，不做串口输出或分配。PXADB `PERF START/STOP/READ/CLEAR` 在停止后分批取回原始光栅耗时和相邻新 GameRender 帧的面板完成间隔；满缓冲、Surface 更换另行报告。Pai 的面板传输完成处记录帧 ID，避免把同一帧的 UI 重组当作新游戏帧。Korvo 的直扫路径在面板完成等待点记录帧 ID；LVGL 合成路径把提交帧的 ID 与输入时间戳留到下一次面板完成等待点，并用中断保存的 32 位微秒时间戳还原实际完成时刻，避免把合成完成误记为显示完成。脚本 `tools/measure-device-voxel.py` 可对已安装的签名包预热 15 秒、采样 8 秒、取原始数据和截图、停止并清理探针。Pai 测试固件 app bin SHA-256 为 `a817a7aa985e0dfee0a2107114b9add1f4e1034b7772affb35af1f132aab318a`，两包使用同一固件和板卡。

复测时先在具有 `perf-raster` 能力的测试固件上用 `python3 tools/pxadb/pxadb.py package install build/resource-refactor/voxel-frozen-final-esp/pxa-voxel-source.pxa --port /dev/ttyACM0 --yes` 安装对应包，再运行：

```sh
python3 tools/measure-device-voxel.py --port /dev/ttyACM0 \
  --package build/resource-refactor/voxel-frozen-final-esp/pxa-voxel-source.pxa \
  --output build/resource-refactor/voxel-frozen-final-old-repeat \
  --wake-tap 148 200 --unlock-swipe 148 230 148 20 \
  --unlock-duration-ms 600 --unlock-steps 12
```

新版换为 `pxa-voxel-current.pxa` 并改输出目录。脚本只记录本地包的哈希，安装动作需先完成。

Pai 上先运行旧包三轮、新包三轮，再重装旧包复测一轮。全部 7 轮均通过，每轮取 99～101 个光栅样本与 98～100 个完成间隔，溢出和 Surface 更换均为 0；所有截图的 RGB565 SHA-256 均为 `0a2f3d8818456664ef64d748e80b001e69357c33f8087c0936fcef8c0ae1ccb6`，各采样窗绘制列表恒为 30384 B、覆盖像素恒为 122980。前三轮旧包对前三轮新包的跨轮中位数：光栅 p50 为 30607→30906.5 μs（+0.98%），p95 为 32685→32877 μs（+0.59%）；面板完成间隔 p50 为 71436→71517 μs（+0.11%），p95 为 95412→95403 μs。最后一轮旧包的光栅 p50/p95 为 30591/32101 μs，完成间隔为 71410/95417 μs，未显示持续漂移。逐轮原始数组、像素、设备日志和包摘要在 `voxel-frozen-final-{old,new}-{1,2,3}/` 及 `voxel-frozen-final-old-4/`。这证明该固定同输出场景没有约 5% 的可重复 Host 光栅回退，但也**没有证明性能提升**；完成间隔是帧间周期，不是输入到显示的延迟，固定画面不能代表全部动态玩法与长时间负载。

测完后卸载临时 Voxel 包，Pai 原 0x10000 起 4 MiB 镜像已写回，esptool 报 `Hash of data verified`。恢复后的 HELLO 仍为 `202d179-dirty`，无测试固件 `memory,perf-raster` 能力；原四个应用清单与刷机前逐字节相同，均 inactive。记录在 `voxel-frozen-final-{uninstall,restore}.log`、`voxel-frozen-final-{doctor,packages}-restored.txt`。本节解决同输出实机逐帧性能对照；完整 native 内存归类、长时动态场景与未覆盖的设备生命周期矩阵继续按目标文档执行。

本轮 ESP Host sanitizer 回归、Pai 固件、Korvo 固件和 Sensecap 固件均通过。Korvo 最终合成路径修复后的 app bin SHA-256 为 `f2b0d9b6230fbe93c36bd3f6accb5f069895a01e91ddce67fb14ac2c55ab3c55`，Sensecap 为 `170794bf302dc514660bfbca866d85d6cd987136e86e30d41304718ebc5ef525`；Sensecap 未刷写。完整 `tools/test-pxa-resources.sh` 也通过，涵盖 Core/WAMR sanitizer、ESP Host sanitizer、包工具、真实应用、LVGL 图片、低预算、共享存储停顿和退出；日志在 `voxel-frozen-final-regression-driver.log` 与 `voxel-frozen-final-regression/logs/`。PXADB 主机 56 项测试、测量脚本语法检查以及根工程和 `deps/pxa-system` 的 `git diff --check` 均通过。

Korvo `/dev/ttyUSB0`、2000000 baud 上也以原先安装的 Voxel 0.2.0 包进行了探针冒烟验证，未更换安装包，故不能与 Pai 的旧/新包数据比较。第一次点击 NEW GAME 早于菜单加载完成，截图仍是主菜单，0 帧结果作废。延后点击后进入游戏，Host 光栅已有 73 帧，但旧版探针的面板完成数为 0，证明该设备实际使用 LVGL 合成路径；该失败记录在 `voxel-frozen-final-korvo-smoke-retry/`。接入上述合成帧完成关联后，相同原安装包在真实 800×480 游戏画面取得 90 个光栅样本和 89 个完成间隔，光栅 p50/p95 为 71069/81367 μs，完成间隔 p50/p95 为 85413/106768 μs，溢出与 Surface 更换均为 0；原始数组、设备日志和 RGB565 截图在 `voxel-frozen-final-korvo-composed-smoke/`。这只验收 Korvo 合成路径测量器及应用运行，**不是** Korvo 的同输出新旧版本性能对照；原安装包的源码摘要仍未知。

Korvo 测试结束后写回原 0x10000 起 4 MiB app 镜像（SHA-256 `d9fe9f546d4f75ee7bbc52096929aa2ab3578c4f888245f84c3a7e9e68c36fc2`），esptool 报 `Hash of data verified`。恢复后的 HELLO 为原版 `e6bb9d9-dirty`，不含测试固件的 `memory,perf-raster`；原四个安装应用清单与刷写前逐字节相同且均 inactive。日志与核对结果在 `voxel-frozen-final-korvo-{restore.log,doctor-restored.txt,packages-restored.txt}`。

## Pai 同输出旧—新—旧内存对照

`tools/measure-device-voxel.py` 现在在有 `memory` 能力的固件上同步保存启动前、15 秒预热后、8 秒帧采样后以及停止后的 ESP heap 和共享资源预算快照。复用上一节相同的 Pai 测试固件与旧/新冻结游戏签名包，按旧、新、旧顺序安装和运行，各轮完整 RGB565 截图 SHA-256 都是 `0a2f3d8818456664ef64d748e80b001e69357c33f8087c0936fcef8c0ae1ccb6`，绘制列表均为 30384 B、覆盖像素均为 122980，采样无溢出和 Surface 更换。原始命令/设备日志与报告位于 `voxel-frozen-memory-{old-1,new-1,old-2}/`；`tools/compare-device-voxel-memory.py` 校验固件、包身份、截图与绘制量后生成 `voxel-frozen-memory-comparison/report.json`。

第一轮旧包属于测试固件启动后的冷运行，其启动前 SRAM/PSRAM 空闲为 45267/7204996 B，不能和后来已预热的新版直接作净值对照。新版与回装旧包的启动前空闲分别为 SRAM 38639/38675 B、PSRAM 7184144/7183392 B，接近；预热后新版与回装旧包分别为 SRAM 35919/27175 B、PSRAM 4621444/4365768 B。按各自“启动前空闲减预热后空闲”计算，回装旧包运行增加占用 SRAM 11500 B、PSRAM 2817624 B，新版为 SRAM 2720 B、PSRAM 2562700 B；**新版在该固定玩法的相对增量少用 8780 B SRAM 和 254924 B PSRAM（约 8.6/249 KiB）**。这覆盖整个应用及其伴随系统分配，不是仅资源缓存的节省，也不是启动全程的同时存活峰值。固件全局 `minimum_free` 跨各轮累计，不能拿它比较两个包的独立峰值。

相同预热点的共享预算旧/新内部为 37164/28940 B（新版少 8224 B），外部为 56256/74348 B（新版**多 18092 B**）。预算已包含在 ESP heap 中，不能与前述净差相加。旧/新 ESP AOT 文件大小 389552/341656 B、Wasm 文件大小 211695/166961 B，属于包文件长度，不能直接当作实际驻留减量；另见前文单独测得的 Guest 线性内存高水位。新版 2 秒停用快照的共享预算回到启动前，旧包也回到相同预算；回装旧包停止后 PSRAM 空闲在 2 秒采样时尚未恢复，稍后的空闲值回升至 7181764 B，不能把早期差额误判为永久泄漏或完全归还。

测试后卸载临时 Voxel 包并恢复 Pai 原 4 MiB app 镜像，esptool 报 `Hash of data verified`；HELLO 恢复 `202d179-dirty` 且无 `memory,perf-raster`，原四个应用清单与测试前逐字节一致、均 inactive。日志及恢复后查询在 `voxel-frozen-memory-pai-*`。这轮补上同输出实机稳态堆净变化证据；完整 native 分类、同时间轴瞬时峰值以及长时动态负载仍需继续验收。

## ESP Surface 自有 PSRAM 分类记账

上一节共享资源预算没有覆盖 Surface 后端直接向 ESP 堆申请的帧缓冲、光栅 scratch、绘制列表邮箱与按需性能探针。现在这些块按 `heap_caps_get_allocated_size()` 返回的分配块大小分别累计，邮箱扩容时先计新块、释放旧块后再扣除，因此短时重叠进入 `peak_total`；只在创建、扩容、释放和显式诊断时工作，不在每帧像素循环取堆锁。PXADB `MEMORY` 新增 `scope=surface`，客户端 JSON 提供 `surface.frame/scratch/mailbox/probe/peak_total`。这些是 ESP heap 的子集、在共享资源预算之外，既不能再加到 heap used，也不能称为完整 PXA native 占用；`peak_total` 是自本次固件启动以来的 Surface 自有块高水位，**不是**每个应用或整机的同时峰值。

ESP Host sanitizer 的 Surface 测试覆盖普通帧缓冲、三类 game-render scratch、邮箱扩容重叠、探针启停、关闭中渲染及显示租约晚释放后的分类归零；PXADB 56 项主机测试覆盖新增作用域解析。Pai、Korvo、Sensecap 三板固件按各自 IDF 环境顺序构建通过，app bin SHA-256 依次为 `7c0a285e2da7b8cfd47a7ef247e16b48b7b8c394e9612be79b6dd1b0c56ddcfd`、`3a4c989e3b4e87bff4128c732c134729ed87a2b66b22e05c71e5b26b29930748`、`aaa7ae2f7434ea9ea2ba099555cc97f8a121483ec01be52afdebe086e2b94eb2`。原始日志在 `surface-native-memory-{host-final-test,pxadb-test,pai-build,korvo-build,sensecap-build}.log`。

Pai 上用同一测试固件依次运行冻结画面的当前/旧签名 Voxel 包，两轮完整 RGB565 SHA-256 均为 `0a2f3d8818456664ef64d748e80b001e69357c33f8087c0936fcef8c0ae1ccb6`，采样窗绘制列表恒为 30384 B、覆盖像素恒为 122980。两包预热后 `surface` 均报告帧缓冲 426240 B（3×296×240×RGB565）、scratch 142080 B、三份 DrawList 邮箱 147456 B，合计 **715776 B**；探针启动后额外 2112 B，停止应用并清理探针后四类当前占用均归零。原始快照、逐帧数据、截图和串口日志位于 `surface-native-memory-pai-{current,old}/`。这排除了“新版通过缩小 Surface 帧缓冲取得上一节 PSRAM 净收益”的解释，也暴露出两版共有的约 699 KiB Surface 驻留成本；其是否能安全减少需要另以同输出、帧引用和性能对照验证。

新固件首次刷写后约 10 秒才宣告 PXADB 就绪，过早连接的 HELLO 失败不能算固件不可启动。一次带主动复位的启动日志在约 8.8 秒报告 CPU0 main 任务的 IDLE0 看门狗告警，随后 Host 和 PXADB 正常启动并通过上述应用测试；原始日志为 `surface-native-memory-pai-boot-live-reset.txt`。该告警发生在本轮新增 Surface 分配记账被调用之前，堆栈顶层位于 LittleFS 分区读取；是否为既有启动耗时问题及其重现频率尚未验收，不能直接归因或忽略。测试包卸载后 Pai 原 4 MiB app 镜像写回且 esptool 报 `Hash of data verified`；恢复 HELLO 无 `memory,perf-raster`，应用列表按 ID 排序后与刷写前逐项相同且均 inactive。恢复日志在 `surface-native-memory-pai-{restore.log,doctor-restored.txt,packages-restored.txt}`；Korvo 和 Sensecap 仅构建，未刷写。

随后也在 Korvo `/dev/ttyUSB0`、2000000 baud 验证这份固件。先用 ROM 读取 0x10000 起 4 MiB，备份 SHA-256 为 `d9fe9f546d4f75ee7bbc52096929aa2ab3578c4f888245f84c3a7e9e68c36fc2`，与此前保存的原镜像完全相同；只刷写本轮 Korvo app bin，esptool 写入哈希验证通过。设备原安装的 Voxel 0.2.0 包在 800×480 游戏场景预热后，Surface 分类为 frame 2304000 B（3×800×480×RGB565）、depth scratch 768000 B、三份 DrawList 邮箱 147456 B，合计 **3219456 B**（约 3.07 MiB）；开启探针时增加 2112 B。8 秒内记录光栅 99 帧、面板完成间隔 98 个，溢出与 Surface 更换均为 0，光栅 p50/p95 为 63435/72937 μs、显示间隔 p50/p95 为 85408/106739 μs；绘制列表恒为 15256 B、覆盖像素在 298195～298207。这是单一原安装包的当前固件数据，不能作为同输出版本性能对比。

采样末尾的 800×480 原始 RGB565 截图在 UART 传输中出现 Base64 损坏，故 `surface-native-memory-korvo-voxel/report.json` 明确为 `passed=false`，不将整轮脚本算作通过；此前取得的原始帧耗时和内存快照仍可单独检查。应用清理后独立 `MEMORY` 查询显示 Surface 四类当前占用均为 0，预算拒绝与分配失败均为 0；重新进入游戏后单独捕获的设备 JPEG `surface-native-memory-korvo-voxel/game.jpg` 可见 Voxel 场景，再次停止后四类仍为 0。原始串口、快照、探针数组及单独清理结果保存在 `surface-native-memory-korvo-voxel/` 与 `surface-native-memory-korvo-after-stop.json`。测试结束后完整写回本次设备实读的 4 MiB 原镜像，esptool 报 `Hash of data verified`；HELLO 回到原版 `e6bb9d9-dirty`，安装应用按 ID 排序与测试前完全一致，测试前处于前台的 PXA ABI Lab 已重新启动并报告 `active=1`。备份、刷写、恢复及列表核对日志为 `surface-native-memory-korvo-{backup,flash,restore}.log`、`surface-native-memory-korvo-{doctor-test,doctor-restored,packages-before,packages-restored}.txt`。Sensecap 仍仅构建，未刷写。

## 已落地：Guest SDK 去掉 v0 与版本后缀

- `sdk/guest-c` 现在只有一套 API。`pxa_v0.h` 与所有 `PXA_GUEST_LEGACY_V0` 分支删除，`pxa_v1_*.h` 并入无版本号的服务头（`pxa_core.h`、`pxa_audio.h`、`pxa_ui_wire.h`、`pxa_clock.h` 等），函数与宏同样去掉 `v1` 后缀。协议层版本保留：导入仍是 `pxa.core.v1`，包清单仍是 `min_sdk`/`target_sdk`/`compile_sdk`。
- 生成器改为双模式。Host 副本继续同时发出 v0/v1 信封（Host 内部 12 字节 mailbox 仍依赖 v0 codec）；Guest 副本只发当前信封并使用无版本名，文件为 `pxa_wire.h`。两份生成头的守卫与共享 helper 互斥，混用一个翻译单元不会重复定义。
- Guest 头的包含守卫统一加 `PXA_GUEST_` 前缀；Host C API 与 Guest SDK 混用时重叠的常量（状态码、Core opcode、GameRender scratch）改为先定义者生效，Guest 的 `pxa_asset_info_t` 改名 `pxa_asset_descriptor_t`。
- 测试：删除已被当前 ABI 用例覆盖的旧用例（device、game_render、ipc、permission、storage、system），把 fs、log 与 arcade 夹具迁到当前 API；WAMR 夹具现已迁到当前 Guest SDK，旧的 `pxa_v0_fixture.h` 和 `pxa.core.v0` 导入绑定已移除。
- 应用：`local/pxa-apps` 全部换名（含 `.inc` 源码），并修掉 plane_shooter 的旧状态语义、arcade 模块的头文件依赖与 voxel-craft 的日志长度宏。
- 验证：`scripts/test-host.sh` 63/63 通过；`tools/package/test_guest_sdk.sh`、`test_package_tool.sh`、`check_spec.py`、`test_wire_codecs.py` 通过；`local/apps.toml` 覆盖的 18 个应用 simulator 包全部重新打包成功。
- 已知仍未收敛：`wasi-undeclared-random` 这个负例会被新的导入校验拒绝，但 `test_cmake_wasi_apps.sh` 仍按“打包成功”断言；顶层同时开启 `PXSYS_BUILD_TESTS` 与 `PXSYS_BUILD_SIMULATOR` 时 `pxsys_resources_test` 目标重名。
