# PXA 流式 HTTP 实施与验证（2026-10-14）

本轮完成网络服务 0.4、Core 服务 0.2 的资源等待，以及 C++ HTTP 客户端。已有 `pxa_call` / `pxa_io` 导入不变。主要实测收益是提前取得响应头、有界传输和更低的下载峰值内存；正文吞吐需结合下列原始结果判断，不能由此宣称游戏 FPS 提升。

本文保存首轮测量。后续的调度优化、复测及实际浏览器验证见[后续迭代报告](pxa-http-stream-followup.zh-CN.md)。

代码基于产品仓库 `69ad37c`、pxa-system `07c2a08`，测试包含本轮未提交修改。设备为 pai-touch（ESP32-S3，296×240，MAC a4:cb:8f:d5:d4:f0，当前枚举为 `/dev/ttyACM3`）。仅更新 factory 应用分区，原应用分区完整备份在 `local/http-stream-validation/device-original-app.bin`；没有重刷分区表、NVS 或应用数据。

## 可复现工作负载

同一电脑启动 `deps/pxa-system/simulator/desktop/tests/http_fixture.py --bind 0.0.0.0 --port 31877`。每个正文是固定字节序列 `(offset * 31 + 7) & 255`，逐字节验证；响应头发送后固定等待 80 ms。基线均下载 `/baseline` 的 262144 字节，完整功能测试下载 `/large` 和 `/chunked` 的 1048576 字节。

测试应用见 [http-stream 示例](../deps/pxa-system/sdk/guest-cpp/examples/http-stream/README.zh-CN.md)。设置 `PXA_HTTP_FIXTURE_URL=http://电脑局域网地址:31877`，`PXA_HTTP_TEST_CASE=buffered/streamed/paired` 选择对比；空值运行功能验证。使用 C++26、O3，ESP AOT opt=3、size=0。不同模式同时构建时必须使用独立 `PXA_BUILD_CACHE_DIR`。

分别安装基线包，然后运行：

```sh
python tools/measure-device-http.py --port /dev/ttyACM3 --case buffered --output local/http-buffered
python tools/measure-device-http.py --port /dev/ttyACM3 --case streamed --output local/http-streamed
```

每次安装后运行对应命令。完整包使用 `--case full`。`paired` 包也用 `--case full --repeat 1`，同一启动内先预热两条路径，再交替运行各六次。

两个独立基线包同时保留两条实现，只改变初始选择；Wasm 文件同为 104506 字节、AOT 文件同为 287060 字节，设备映射各 327680 字节，线性内存均固定 131072 字节。相同 UI、字节校验和结果记录。正文计时在文件写入之前结束，截图在内存监测结束后抓取。

每轮使用 ESP-IDF `MEMORY START/STOP` 的局部最低空闲值，包含两次采样间已释放的短暂分配。峰值增量 = 启动前空闲 − 本轮最低空闲；稳定增量 = 启动前空闲 − 完成后空闲。历史累计最低值和历史网络最大响应量不参与峰值比较。首次网络线程初始化单列。

## 同负载真机结果

旧、新包各自重启设备，等待 40 秒让 Wi-Fi 连接，再连续运行三次。首次请求包含网络服务的延迟初始化；权限已提前授予，不将权限弹窗耗时当作传输耗时。

| 路径 / 轮次 | 响应头 ms | 首块 ms | 完成 ms | SRAM 峰值增量 B | PSRAM 峰值增量 B | 完成后 PSRAM 增量 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 旧缓冲 / 首轮，含网络初始化 | 962 | 962 | 996 | 6204 | 1095984 | 577936 |
| 旧缓冲 / 第 2 轮 | 944 | 944 | 979 | 5528 | 1075892 | 552832 |
| 旧缓冲 / 第 3 轮 | 960 | 960 | 993 | 5492 | 1073704 | 552888 |
| 新流式 / 首轮，含网络初始化 | 196 | 309 | 1236 | 6164 | 839056 | 576024 |
| 新流式 / 第 2 轮 | 47 | 146 | 1157 | 5492 | 819768 | 552904 |
| 新流式 / 第 3 轮 | 35 | 156 | 1109 | 5528 | 822800 | 552920 |

六轮全部通过内容校验。预热后的响应头在 35–47 ms 可用，旧路径需要等正文完整到达。PSRAM 峰值增量由约 1049–1051 KiB 降至约 801–804 KiB，减少约 245–250 KiB；包含首次初始化时减少约 251 KiB。SRAM 峰值相当，预热后均为 5492–5528 B。完成后的 PSRAM 驻留接近，约 540 KiB，主要是相同的应用代码和线性内存。

独立运行中旧路径完成时间 979–996 ms、新路径 1109–1236 ms，新路径没有总耗时优势。后附同一次启动内交替测试，进一步减少冷启动和顺序影响。

## 同一次启动内交替对比

首对用于预热，随后各六次下载 256 KiB，全部逐字节验证通过。没有重新加载包或重置 Wi-Fi：

| 路径 | 响应头中位数 ms | 首块中位数 ms | 完成中位数 ms | 完成时间范围 ms |
| --- | ---: | ---: | ---: | ---: |
| 旧缓冲 | 1170 | 1170 | 1258 | 1001–3293 |
| 新流式 | 41.5 | 152 | 1388.5 | 1286–1711 |

流式响应头中位数降低约 96.5%，首次正文降低约 87.0%。正文完成中位数增加约 10.4%；本轮不宣称正文吞吐更快。明确的收益是更早的可用数据和更低的峰值内存。不同轮次有明显 Wi-Fi 波动，不能用最快单轮代替总体结果。

本轮还试验了 2 KiB 与 4 KiB 单次传输上限。2 KiB 的另一组交替实验中旧、新路径完成中位数分别为 1100.5 / 1441.5 ms；最终采用 4 KiB，在原有环形缓冲内摊薄每块通知和队列处理，不增加正文数组、库缓冲或线程栈。两组不是同一无线条件下的受控 A/B，不能把差异全归因于分块调整；原始数据和对应固件哈希均保留。预算很小的流式路径仍有额外调度成本，后续可单独评估更大的可选缓冲和公平连接调度。

[交替验证截图](benchmarks/http-stream/2026-10-14/paired.png) 和 results.json 保存完整 14 次计时（含预热）。

## 实施选择与内存账目

| 改动与原因 | 收益 | 内存变化 | 验证 |
| --- | --- | --- | --- |
| 总传输预算与驻留缓冲分开 | 4 KiB 缓冲可持续读取 1 MiB，并及时消费正文 | 256 KiB 下载正文驻留降为 4 KiB；额外响应头 1 KiB、ESP 交换状态 128 B | 真机、模拟器逐字节校验，慢消费者及总量超限测试 |
| 一次性、可靠的资源 readiness 等待 | 无每帧网络轮询，关闭/取消/错误均能唤醒；处理伪唤醒 | ESP 复用已有 lease 表，每槽仍 32 B；每块 IO 不增加 Task 池槽 | 取消、token 重用、错误所有者、后台取消和晚到事件测试 |
| 响应头按实际字节持有、重复字段保留 | 大 Cookie 和大小写不敏感查询可用，避免固定字段数组 | 空 Response 无堆分配；仅按实际头大小分配 | SDK 的 600 字节值、重复 Set-Cookie、临时查询字符串测试 |
| 复用已有网络线程和四个请求槽 | 不为流式请求引入第二套常驻线程或槽表 | 固定槽元数据仍 4064 B；线程栈复用 | ESP 后端 mock 的创建、分配及清理计数 |
| 最多 4 KiB 一次读写，合并 owner 通知 | 摊薄 512 字节分块的任务唤醒开销 | 不增加库的 512 B 接收缓冲、环形缓冲或栈上正文数组 | 同负载真机复测、满缓冲背压回归 |
| 去掉可推导指针/容量，核对完成编码上界 | 抵消新增后端回调的元数据 | Xtensa 网络工作区减少 8 B；64 位减少 8 B | 前后结构布局及全部核心回归 |
| 资源权限与共享并发额度 | 撤销权限关闭传输；旧、新请求不能各占一套额度 | 使用已有 authority、请求和句柄表 | 服务级配额、撤销和打开失败回收测试 |

结构布局（ESP32 32 位）：net service 104→108 B，pending 48 B、stream 20 B 不变，完成工作区额外预算 160→144 B；lease service 48 B、entry 32 B 不变。pending 表要求 8 字节对齐，服务头按 8 字节取整后实际为 112 B（修正之前紧接 108 B 头的潜在未对齐访问）；网络工作区净变化 8−16=−8 B。桌面 64 位净变化 8−16=−8 B。

Guest 线性内存、基线代码映射相同，PXA 独立 Surface 的 frame/scratch/mailbox 分配均为 0；系统 LVGL 显示缓冲没有因本轮网络改动增加。Surface 统计为 0 不代表设备没有显示帧缓冲。非网络应用不建立 HTTP 交换或增加默认正文缓存；链接新 API 的实际代码映射仍应按具体应用测量，不能以这个测试包保证任意应用的映射页数不变。

桌面产品模拟器原先没有该 Core 等待服务，现在仅在声明网络服务的应用中建立四槽等待工作区，64 位构建实测为 256 B；这是模拟器新增的固定开销，另有 Host 状态指针和唤醒标记，不应隐去。ESP 本来已有该池，未增加表容量。

## 功能与回归

最终真机完整包连续启动三轮，均通过已知长度下载、chunked 下载、提前响应头、重复 Set-Cookie、取消、100 KiB 上传、尚未上传时提前返回 413。1 MiB 下载最终耗时范围：已知长度 4012–5049 ms，chunked 4245–4993 ms；头返回分别 36–90 ms、33–35 ms。此处只描述流式工作负载，不能与 256 KiB 基线直接比较吞吐。

原生 27 项核心回归、C++ SDK 完整 host 套件、C++26 HTTP 定向检查、ESP adapter host 套件、桌面 HTTP/旧响应头两项检查全部通过；Core 和新 HTTP 测试的 ASan/UBSan 检查通过。最终真实 Wasm/AOT 模拟器应用也通过全部流程（1 MiB 已知长度 526 ms、chunked 595 ms）。模拟器和设备网络、调度不同，不把它们当作同一硬件性能比较。

桌面原生同负载基线：旧接口首次正文 84 ms、总计 86 ms；新接口响应头 2 ms、首次正文 82 ms、总计 106 ms。小型本地传输的新路径没有总耗时优势，收益在更早的响应头和内存。慢消费者、接收满时取消、响应超预算、重定向交回 Guest、提前拒绝上传及矛盾 HTTP framing 均通过。

调试日志队列容量较小，曾丢失最后的完成消息；复核截图证实应用已成功。验证应用增加带本次启动时间的结果文件，并在完成时关闭文件，避免文件系统尚未提交或误读上轮结果。工具同时保留日志和截图；不能因为日志没有 PASS 就推断传输挂起。

完整数值、二进制 SHA-256、测试日志及截图见 [验证产物](benchmarks/http-stream/2026-10-14/results.json)，[旧路径截图](benchmarks/http-stream/2026-10-14/buffered.png)、[新路径截图](benchmarks/http-stream/2026-10-14/streamed.png)、[完整功能通过截图](benchmarks/http-stream/2026-10-14/full.png)。逐次内存采样原始记录保留在 `local/http-stream-validation/final-4k-*/report.json`，交替数据在 `local/http-stream-validation/quantum-4k-paired/report.json`。最终测试包已经卸载，保留原有 Jump Jump 3D 和数据，返回系统主页面；没有修改持久设备设置。

## 回归命令

在产品仓库根目录运行核心和 ESP 后端回归：

```sh
cmake -S deps/pxa-system/libpxa -B /tmp/pxa-http-lib -DPXA_BUILD_ADAPTERS=OFF -DPXA_BUILD_TESTS=ON
cmake --build /tmp/pxa-http-lib -j4
ctest --test-dir /tmp/pxa-http-lib --output-on-failure
bash deps/pxa-system/tools/package/test_guest_cpp.sh
bash firmware/components/pxa/tests/test_host.sh
ctest --test-dir deps/pxa-system/build -R 'pxsys_desktop_(http_stream|net_headers)_test' --output-on-failure
```

SDK 脚本需要 C++26 工具链。桌面测试需先按项目说明构建带 libcurl 的产品模拟器。ASan/UBSan 使用独立核心构建目录，设置 `CMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g`，运行 `pxa_core_test` 和 `pxa_http_stream_test`。各测试的最终输出随验证产物保存。

## 已知限制

Guest/Host 边界仍有一次受边界检查保护的正文复制；这里没有实现跨 Wasm 的零拷贝。改掉的是全量正文积累、每块控制消息编码和额外协程分配。

ESP 的连接、DNS/TLS 初始化及旧缓冲请求仍可占用共享网络线程。正文背压已分块，但尚未实现连接阶段的公平并行调度或连接池。实际设备本轮使用受控局域网 HTTP；TLS 连接沿用 Host 库和证书策略，未在本轮做公网 HTTPS 真机性能对比。

目前上传长度必须已知，受 ESP API 限制为 INT32_MAX；响应头总预算最高 4 KiB。重定向由 Guest 重新发起并检查权限，未增加自动解压、Cookie jar、HTML 布局、JavaScript 或浏览器页面缓存。这里交付的是浏览器和网络应用可复用的底层，不等同于完整浏览器。游戏帧率和触摸延迟不在这组网络测试中，未据此作收益宣称。
