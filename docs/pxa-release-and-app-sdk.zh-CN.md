# PXA 版本、兼容性与独立开发套件发布方案

状态：2026-10-09 代码审计后的设计方案。已有独立 C++ 开发包和仓库外构建验证；统一发行元数据、完整开发套件、可重定位模拟器及正式发布流水线仍待实现。本文中的 `0.2.0`、`1.2.0`、新 CLI 命令及新 JSON 字段均为设计示例，不代表已经发布或可直接执行。

## 1. 推荐的总体规则

现阶段采用一个 PXA 发行号：同一份 `pxa-system` 源码发布的 Guest C/C++ SDK、Host 库、CLI、模拟器及官方开发套件使用同号。继续遵循 `local/PLAN.md` 中 SDK 跟随 PXA 发布版本的决定，不再为 C、C++ SDK 各增加一套版本。

**同号用于提供经过验证的配套下载；App 的运行兼容性由 Core、Service、Package、WASI/Wasm 特性及 AOT ABI 决定。安装器不能要求“构建 SDK 发行号等于设备 PXA 发行号”。** 新 SDK 构建的 App 可以运行在旧设备上，前提是实际所需能力及 Artifact 都满足；新设备也应继续运行已承诺兼容的旧 App。

板型固件有自己的发行版本，同时报告它嵌入的 PXA 发行号和源码提交。仅修复 SDK 不要求给每块板发布新固件；仅调整板驱动也不要求升级 PXA 协议。App 版本、存档格式版本同样独立。

开发者默认下载一个完整的 **PXA DevKit**，内含 SDK、工具链、打包/调试 CLI 和模拟器；也提供拆分下载与离线包。不需要 PXA/产品源码、WAMR/LLVM 源码或 ESP-IDF。Host SDK 仅供系统集成者使用。

## 2. 当前代码实际上如何管理版本

| 项目 | 当前实现 | 审计结论 |
| --- | --- | --- |
| PXA 根 `VERSION` | `deps/pxa-system/VERSION` 为 `1.0.0-dev` | C++ 开发包直接复制它；尚未成为全仓库统一来源 |
| CMake、Host C 库 | system/libpxa `project(... VERSION 0.1.0)`；`libpxa/include/pxa/version.h` 也是 `0.1.0` | 与根版本不一致；原生 `SOVERSION` 目前取 CMake major |
| ESP-IDF 组件、PXADB | system `idf_component.yml`、PXADB Python 包为 `0.1.0` | 另有手写版本，不能推断为已验证的一套发行版 |
| Host SDK 打包 | `scripts/package-sdk.sh` 默认 `0.1.0`，安装 Host 库/CMake 元数据 | 名称容易被当成 App SDK；应明确为 Host SDK |
| C++ Guest 开发包 | `tools/package/build_guest_cpp_sdk.sh` 包含 C++ SDK 源码/头、CMake、协议 JSON/生成器、打包器、一个预编译 `wamrc`、`VERSION` | 已支持外部 App 根目录和仓库外消费；不应重新声称“完全没有独立 SDK” |
| 编译器 | `resolve_wasi_sdk.sh` 固定 WASI SDK `34.0`，Linux 下载校验归档摘要 | 已有基础；使用外部安装时主要检查完整性/版本文件，正式锁定还应核对来源摘要 |
| WAMR/AOT | `config/wamr.json` 固定 WAMR/LLVM 提交，`engine_abi=wamr-pxa-aot-v6-core-1` | 已有独立 AOT 兼容标识；预编译工具还需解决共享库依赖及平台分发 |
| Core | Guest 使用 `pxa.core.v1`；`wire.h` 定义 Core `1.0`；包要求验证只接受 major 1 | 新 App 的运行边界已明确；`pxa-core.json` 仍有历史 `0.1` 描述，v1 信封在单独 JSON 中，正式发布前需整理规范入口 |
| 服务 | `protocol_metadata.py` 从服务规范和 Core 服务目录读取并检查一致性；每服务有 major/minor 和 feature bits | Assets 当前 `2.1`、UI `0.6`、GameRender `0.5`、Audio `0.8`，都不是 SDK 发行号 |
| App 包 | 源格式 `pxa-package-source-0.1`；当前生成 Manifest `0.7`；`.pxa` 容器独立版本 | 源清单、Manifest、容器三者不能混为一个版本 |
| SDK 字段 | `min_sdk`、`target_sdk`、`compile_sdk` 为 `[major,minor]` | 表示 Core 要求/行为/构建信息，不是 SDK 下载版本；当前打包器要求同 major 且 `min <= target <= compile` |
| App 更新 | 独立 App 版本字符串、严格递增 `release_sequence`、发布者身份/密钥沿革 | 不靠显示版本字符串排序升级；身份和升级序号不能因换 SDK 而丢失 |
| 模拟器 | 已有 UI、产品 App 运行器、包安装及 PXADB 服务 | 构建/默认资源仍引用产品字体、LVGL、源码目录；目前不是完整的可下载即用产品 |
| 设备信息 | PXADB HELLO/INFO 的 `version` 来自 ESP 应用描述，`pxa` 是 ready/unavailable | 不能把它误认作 PXA 发行号或完整 ABI 清单；部署预检查需增加能力报告 |

现有 SDK 验证记录包含 15 个示例三目标、45 次仓库外构建等证据。这证明外部消费路径已经建立；不证明发布包在无源码、无系统开发依赖、离线环境中全部可用。当前仓库没有正式发布 CI 配置，已有文档也是方案。

需优先解决的实际问题：多个版本来源；完整工具依赖未封装；模拟器不可完全重定位；服务简写默认抬高最低版本；用户无法通过一份设备报告明确判断所有兼容项。不能直接把当前 `1.0.0-dev` 改成正式 `1.0.0` 并宣布稳定。

## 3. 版本的职责与是否需要对应

| 标识 | 示例 | 是否跟随 PXA 发行号 | 对兼容性的含义 |
| --- | --- | --- | --- |
| PXA / Guest SDK / Host 库发行号 | `0.2.0` | 推荐同一源码发行同号 | SDK 源码 API、库 API、官方工具组合的版本；用于安装、复现和变更说明 |
| DevKit / 官方模拟器发行号 | `0.2.0` | 初期同号 | 官方验证组合；之后使用不同组件时需显式锁定并验证 |
| 板型固件发行号 | `pai-touch 2.7.1`，内嵌 PXA `0.2.0` | 独立 | 固件升级/驱动变更身份；设备兼容由其真实能力决定 |
| App 发行号与升级序号 | `1.4.2`，sequence `42` | 独立 | 前者显示给用户；后者控制更新顺序 |
| Core ABI | `1.0` | 独立 | 导入、信封、句柄和生命周期契约；同 major，Host minor 满足最低要求 |
| Service ABI | UI `0.6`、Assets `2.1` | 独立 | 每项服务范围、命令/通知语义、feature bits |
| Manifest / Container / Source schema | 当前 Manifest `0.7` | 独立 | 能否正确解析包/签名/清单；源格式只影响构建工具 |
| AOT engine ABI | `wamr-pxa-aot-v6-core-1` | 独立 | 当前选择器要求与 Host 精确相等，并匹配目标及特性/内存模型 |
| Toolchain bundle 身份 | WASI SDK `34.0` + PXA wamrc recipe + SHA-256 | 独立锁定 | 生成指令、标准库和 AOT 的确切构建来源 |
| Host 原生 C ABI | `host_c_abi=0`（保留当前 soname） | 独立整数 | 本机动态链接兼容；对应 `SOVERSION`，与 Guest 不相干 |
| PXADB 协议、IPC/存档 schema | PXADB1/2、App save v3 | 独立 | 调试工具协商、App 间消息及持久数据的兼容边界 |

回答“SDK 版本和 pxa 库版本要一一对应”：**官方构建来源推荐一一对应，运行要求不一一对应。** 目前项目规模下，独立 SDK 发行号收益小、管理成本大。未来若 SDK 与 Host 团队有不同发行周期，可以新增独立 `sdk_version`，由 DevKit 清单固定组合；不改变 App 运行判定，也不同时推出 C/C++/CLI 多套不必要的版本。

例如 PXA `1.3.0` SDK 只优化编码或增加本地辅助算法，没有新导入、新服务要求、指令要求或不同 AOT ABI，生成的 App 可以在 PXA `1.2.0` Host 运行。C++ 公开 API 在 PXA `2.0.0` 改名，也不意味着已安装旧 App 的 Core 必须升级到 2。

## 4. 发行号、分支和升级规则

发行号按 [SemVer 2.0](https://semver.org/spec/v2.0.0.html) 管理。统一发行的公开契约包含 Guest C/C++ API、公开 Host API、CLI/source schema 和承诺的运行兼容性；不包含内部布局和未承诺的实验 API。

- `0.x` 阶段：PATCH 仅兼容修复；公开能力或实验 API 调整升 MINOR，并列出破坏项。即便还在开发，也承诺不在 PATCH 静默破坏旧 App。
- 稳定阶段：PATCH 为兼容修复；MINOR 为兼容功能及弃用提示；MAJOR 为公开 API/承诺的兼容性破坏。不能把破坏已有 AOT 装载支持的变更当作稳定期普通 MINOR。
- 新增 Service opcode/feature，通常增加该服务 minor；改动既有命令语义或结构且无法兼容，增加该服务 major。服务线上的 major/minor 是协议规则，即便 major 为 0，也要明确承诺其同 major minor 演进规则，不能直接套用“0.x 什么都能改”。
- SDK 本地实现优化不改 Core/Service ABI。Core 通用信封或句柄语义破坏才改 Core major；现有 major 1 硬编码边界升级时需同步装载器、WAMR import namespace 和测试。
- `engine_abi` 变更不必和 PXA major 数字一样，但必须按所造成的公开兼容影响选择 PXA 发行级别。

`main` 做日常开发；必要时维护 `release/1.2` 分支，只回移兼容修复。使用 `vX.Y.Z-rc.N` 候选 tag 和不可移动的 `vX.Y.Z` 正式 tag。nightly 明确标为 `X.Y.Z-dev.N+g<sha>`，脏树加 dirty 标记；不冒充正式包。`+build` 不参与 SemVer 排序，不能靠它给正式版本“偷偷加一”。

建议首次对外先发布 `0.2.0-rc.1`（版本仅为建议），收敛 ABI 规范与分发验收后进入正式版。达到可维护的公开契约、旧二进制回归、工具平台支持和兼容说明后再决定 `1.0.0`。保留每个正式版资产，不覆盖同名文件；错误通过下一 PATCH 修复。

## 5. 唯一来源和两类清单

### 5.1 源码版本来源

`pxa-system/VERSION` 是唯一手写的 PXA 发行号。构建时拆出数字部分供 CMake `PROJECT_VERSION`，完整字符串供 generated header、CLI/模拟器 `--version`、包文件名及设备报告。不能直接把含 `-rc.1` 的字符串塞给要求数字版本的 CMake `project(VERSION ...)`。

从它生成 Host `version.h`、Guest 可查询的构建版本、ESP 组件发行元数据；不再各处手写 `0.1.0`。Host C ABI 独立维护后生成 `SOVERSION`。Core/各服务/包格式从规范生成，WAMR 配方从 `config/wamr.json` 读取，工具链宿主平台/摘要从工具链锁定配置读取。CI 检查这些来源和生成物一致；不把当前所有 `0.1.0` 一次性替换成相同值。

正式发布前整理规范：明确历史内部 mailbox 与 Guest v1 20 字节信封；把已经实现的 v1 envelope 从 proposal 状态转为准确的规范；避免工具误将历史 Core service `0.1` 当成 Guest Core `1.0`。Service 目录、生成代码、Host 注册、SDK 编码器必须共同验证。

### 5.2 `distribution.json`：下载及复现清单（设计）

正式发行生成只读清单，记录完整提交、构建配方、组件摘要、许可证和支持平台。建议结构：

```json
{
  "schema": "pxa-distribution-1",
  "release": "0.2.0",
  "source_commit": "<完整 pxa-system 提交>",
  "components": {
    "guest_sdk": {"version": "0.2.0", "sha256": "<归档摘要>"},
    "devtools": {"version": "0.2.0", "sha256": "<归档摘要>"},
    "simulator": {"version": "0.2.0", "sha256": "<归档摘要>"},
    "toolchain": {"id": "<固定配方 ID>", "sha256": "<归档摘要>"}
  },
  "host_platform": "linux-x86_64",
  "compatibility_profile": "profiles/pxa-default.json"
}
```

完整格式还需 URL/长度、WASI SDK/Clang/libc++/wamrc/WAMR/LLVM 的确切版本和提交、目标 recipe、依赖摘要及构建来源。摘要不能证明来源真实性，分发清单也应通过受信发行签名或可验证的发布证明认证。发行签名与 App 发布者签名分别管理；不在包中分发私钥。

### 5.3 Host 能力档案：安装判定清单（设计）

模拟器和固件由同一规范生成能力报告：PXA 发行号/提交、板型固件版本、Core、各服务版本与 feature bits、Manifest/Container 支持、WASI/Wasm 特性、目标 CPU/ABI、engine ABI、解释器可用性、内存/资源配额及 PXADB 协议能力。

该报告需反映实际构建选项和后端能力，而不是“源码里存在某个头文件”。例如无网络板不能把网络能力标成完整支持。设置一个基准 profile 供所有官方 Host 尽量共同满足，再以板型附加能力和预算补充。模拟器的屏幕 profile 不能被当作其 AOT CPU 目标：模拟 pai-touch 屏幕仍需要本机 AOT 或可运行的 Wasm。

增加 `pxa device info --json` / `pxa doctor --device`（设计命令）读取这份报告。原有 PXADB HELLO 保持可协商；新增结构化查询不必破坏 PXADB1/2。元数据放只读区、查询时有界编码，不新增常驻大型缓存。

## 6. 用户下载什么、如何开发

| 下载物 | 必须包含 | 使用者 |
| --- | --- | --- |
| Guest SDK | C 与 C++ 头文件、C++ runtime 源码、CMake Config/targets、协议/IPC 生成器、模板/示例、API/容量/语言支持说明、版本元数据 | App 开发者 |
| DevTools | `pxa` CLI、Manifest/容器/资源/i18n 工具、导入/内存校验、签名、PXADB、测试与诊断入口 | App 开发者 |
| Toolchain | 固定 WASI Clang/LLD、sysroot/libc++、预编译 PXA `wamrc`、实际运行所需 LLVM 等动态库、固定目标 recipes | App 编译和多目标 AOT |
| Simulator | 预编译运行器/安装器、系统 UI、服务后端、字体、屏幕 profiles、必要运行库及本地 PXADB 支持 | 模拟运行，无硬件也能开发 |
| Host SDK | libpxa/Host 头库、CMake 导出和集成文档 | 固件/Host 集成者；普通 App 不下载 |
| 板型固件 | 固件、PXA/板型元数据、分区/升级说明 | 设备用户；独立于 App 开发包 |

提供 `pxa-devkit-<release>-<host>.tar.*` 作为默认入口，包含前四项；另提供 SDK/DevTools/Toolchain/Simulator 拆分包，共用同份分发清单和内容缓存。轻量在线安装器下载缺失项；离线完整包包含全部依赖。SDK 本身可跨开发宿主机，但带 `wamrc` 的现有 C++ bundle 不是 `any` 平台包，必须按宿主平台标记。

首批正式承诺 Linux x86_64。Linux arm64 当前有 WASI 下载分支，但完整 wamrc、模拟器及依赖未全部验收，不直接宣布完整支持。macOS/Windows 在各自 CI 和运行验收后加入，Windows 路径不能依赖用户另装 Bash/WSL。可另外提供固定镜像的构建容器，镜像不是唯一使用方式。

DevKit 应包含或管理 Python/pyserial/Pillow、OpenSSL、LZ4、CMake/Ninja 和各工具实际动态依赖，避免用户源码编译后端或手工 pip/apt 猜包。对 glibc、图形/音频驱动等必要 OS 基线明确要求；Linux 不应粗暴打包 glibc，而应选最低支持基线构建。字体与第三方库同时带许可证。

建议目录（设计）：

```text
pxa-devkit/0.2.0/
  bin/pxa, pxadb, pxa-simulator
  sdk/guest-c/, guest-cpp/, cmake/, templates/
  toolchains/<recipe-id>/
  runtime/                         # 私有工具运行时及动态库
  share/pxa/profiles/, fonts/, docs/
  distribution.json, licenses/
```

发布资源按安装前缀/可执行文件位置查找；用户数据、模拟器状态、开发密钥及下载缓存放用户数据目录，不写入只读安装目录。支持搬移、带空格路径、多版本共存和外部任意 App 目录；模拟器不在启动时重新编译。

### 6.1 目标用户工作流（下面均待实现）

```sh
pxa sdk install 0.2.0
pxa init hello --language cpp --sdk 0.2.0
cd hello
pxa build --target simulator
pxa run --sim --profile pai-touch
pxa build --target esp32s3,esp32s31
pxa check dist/hello.pxa --device /dev/ttyACM2
pxa device install dist/hello.pxa --port /dev/ttyACM2
pxa device run hello --port /dev/ttyACM2
```

普通用户不需要读寄存器、ESP-IDF、LLVM 或 pxa-wire。提供 C/C++ 模板、VS Code tasks/clangd 配置和离线 API 文档；IDE 是可选项。C++ 保持当前按需模块、C++26 必需特性检测、O3、无异常/RTTI 的已验证配置，并明确标准库不支持的功能。

现有独立 C++ bundle 的真实用法仍见 `sdk/guest-cpp/README.zh-CN.md`；不要在实现 CLI 前把上述命令写进入门文档当成现成功能。

### 6.2 项目锁定、升级与缓存

`package.json` 描述 App 身份、业务版本、升级序号、权限、服务要求、预算和资源；`pxa.lock` 固定 SDK 发行号、分发清单摘要、工具链 recipe/摘要、目标能力 profile/摘要、关键构建选项及容量。源码和 lock 一起提交；不记录机器绝对路径或私钥。

`pxa build` 使用 lock，不隐式跟随 latest；第一次可安装锁定组件，缺失时给出原因。`pxa sdk update` 显式改 lock，并输出源码迁移/最低设备要求变化。安装新 SDK 不改旧项目。与旧设备兼容时允许锁定旧 wamrc 配方，但仅接受 SDK 支持矩阵中经过测试的组合；不鼓励任意混装工具。

C++ SDK 以源码与当前应用一起构建，避免把由不同编译器/容量宏生成的 runtime `.a` 混用；当前 PUBLIC 容量定义传递继续保留。构建缓存键包含 SDK/工具摘要、目标/CPU/ABI、全部影响布局或代码的选项及资源生成器版本，不仅是 SDK 字符串。生成 package 在临时目录验证后原子替换；构建路径允许自由输出，但拒绝会覆写源目录或危险递归删除的情况。

构建来源补全 SDK 版本/摘要、App 源提交/脏树输入摘要、工具 chain、选项、各目标 recipe、协议要求与输出摘要。现有 provenance sidecar 不是签名兼容授权；需要可信的内嵌来源时，可作为有界普通文件纳入签名文件表，不能拿外部 sidecar 替代签名 Manifest。

## 7. App 兼容管理

### 7.1 源码兼容与二进制兼容分开承诺

SDK API 兼容决定升级 SDK 后是否需要修改/重编译源码；Core/Service/AOT 兼容决定已构建包能否运行。Guest runtime 静态进入 Wasm，所以设备不按 SDK C++ 对象布局动态链接。Host 原生动态库则必须检查单独的 C ABI/SONAME。

稳定 API 先在 MINOR 弃用并给替代方法，后续 MAJOR 再删除。实验模块显式列在支持矩阵，允许开发期演进；不能悄悄把稳定 API 标成实验规避承诺。IPC 合同、存档和资源编码的 schema 独立升级；存档迁移采用临时写入/原子替换，升级失败保留旧数据，不能以运行 ABI 兼容为由认定存档也兼容。

### 7.2 安装/启动时的判断顺序

1. 容器/Manifest 可解析，大小/路径/签名正确，App 身份/更新序号允许。
2. Core major 被该 Host 支持，minor 满足 `min_core`（目前二进制字段名仍是 `min_sdk`）。`target_core` 是行为选择，不是 Host 版本上限。
3. 每个实际启用 Component 的必需服务在同 major 的 min/max 范围内，必需 feature bits 均存在。声明了服务不代表用户已经授予权限；授权走现有流程。
4. WASI 导入与声明特性、Wasm 指令及内存模型受到支持。
5. 存在可运行 Artifact：优先目标/engine ABI/特性完全匹配的 AOT；否则只在有 Wasm 且 Host 启用解释器时回退。
6. 组件、线性内存、栈、句柄、任务、帧缓冲和资源预算可接纳；通过静态预算不等于保证每次运行都能分配成功，失败路径仍应有界恢复。

CLI 在部署前解释差异，Host 安装/启动仍做权威检查。不兼容不能先删除旧版本或 App 数据。报告至少区分缺服务/feature、Core 不符、包格式不支持、目标/AOT ABI 不符、解释器缺失、预算不足、签名/发布者问题。现在单个 `UNSUPPORTED` 不足以给用户定位，诊断要增加有界详细原因，不能改变现有成功/失败语义。

### 7.3 不要因换 SDK 自动抬高最低版本

当前字符串服务名默认取构建 SDK 的服务版本，UI 自动依赖也取当前 Window/UI/Clock 版本。因此升级 SDK 可能抬高最低 Host 要求，即使 App 只用旧操作。这是当前代码行为，不是已经解决的问题。

为 `pxa build --compat-profile <profile>`（设计）提供明确的最低运行基线。已知 SDK 命令/feature 记录 introduced-in 版本，生成器和类型化接口据此约束；新增 API 若超出目标 profile，要编译/打包时报错或要求升级 profile。显式服务范围和 feature 的现有机制继续使用；源字符串简写可保留为保守的“当前 SDK 能力”模式，但在最终诊断中显示解析结果。

不承诺靠扫描 Wasm 自动推导所有服务语义：动态命令、raw wire、第三方库和 IPC 不能可靠推导。运行基线由开发者声明、生成代码校验及旧 Host 实测共同保证。可选能力采用显式查询/分支和降级路径；真正必需能力写入签名要求，不能把不支持时会崩的能力标成 optional。

下一版源 schema 可新增清晰的 `min_core` / `target_core`，旧 `min_sdk` / `target_sdk` / `compile_sdk` 继续读并规范化；两套别名冲突时拒绝。二进制 tag 7/8 保持现有语义，工具自动记录 compile Core 和真实 SDK 发行号。单纯字段改名无需破坏现有包。

### 7.4 AOT 升级策略

当前 `artifact_select.c` 精确匹配 target、engine、engine ABI，同时检查 required features 和 memory model；应保留这条可靠规则。WAMR 上游也说明 AOT 格式号并不保证所有细节兼容，生产建议配套 compiler/runtime。见 [WAMR AOT 兼容说明](https://github.com/wasm-micro-runtime/wasm-micro-runtime/blob/main/doc/build_wasm_app.md#aot-compiled-module-compatibility-among-wamr-versions)。

PXA 的 engine ABI 必须覆盖实际装载/调用契约，包括定制 WAMR、relocation、运行库 helper、目标调用约定及相关构建选项。编译器提交变化记录在 recipe 中；不能仅因 LLVM 版本不同就断言不兼容，也不能因 AOT 格式号相同就沿用 ABI。只有新旧产物/Host 回归证明契约不变时才保持旧 ABI；未验证的契约变化使用新 ABI。

默认固件不为兼容额外常驻多套引擎或 JIT，不因此增加设备内存。更新新 ABI 前，先提供对应 App 重打包版本并检查全部已安装必需 App；支持旧 ABI 或 Wasm 的情况须有测试依据。AOT-only 游戏不会自动获得回退；不建议在设备上临时 AOT 编译。Wasm 可携带用于分发/重打包，但解释执行可能显著降低 FPS，UI 必须报告所选执行方式。

同一 `.pxa` 可包含多个目标 Artifact。若以后允许同目标多 engine ABI，也需逐项验证清单唯一性、排序、大小预算与选择器，不能未经测试就当成既有承诺。安装时只保存选中 Artifact 的优化需要保持签名可验证、包复原及更新一致，列为后续能力，不作为首版要求。

### 7.5 必须发布的兼容矩阵

| 组合/变化 | 目标行为 | 必须验证的内容 |
| --- | --- | --- |
| 旧正式 SDK 的旧包 → 新兼容 Host | 继续运行 | 使用封存的原包，不能只用新 SDK 重编译旧源码 |
| 新 SDK → 旧 Host，保持旧 profile/AOT ABI | 可运行 | 实际新工具生成包在最低承诺 Host 上启动/功能测试 |
| 新 App 使用新服务命令 | 旧 Host 明确拒绝或显式降级 | feature/版本声明准确，已安装旧包保留 |
| 同发行号但板型缺服务/预算小 | 不能因同号强行允许 | capability/预算差异和明确错误 |
| AOT ABI 不匹配，有 Wasm/解释器 | 明示回退 | 实际运行、指令/WASI支持、体验差异 |
| AOT ABI 不匹配，AOT-only | 安装或激活拒绝 | 保留旧应用与数据，不执行错误代码 |
| Guest C++ API 改动 | 源码可能要迁移；旧包按 ABI 继续检查 | 源码迁移用例和封存旧二进制分别测试 |
| App 更新/固件回滚 | 按身份、序号及数据格式处理 | 存档迁移失败、重启、断电、旧 Host 不支持新存档的处理 |

建议稳定期至少自动覆盖当前 minor、上一 minor 和当前 major 的基准旧发行包，支持时限另行明确并发布 EOL 日期。在 major 支持范围内承诺持续兼容的 Core/Service 不得因缩小 CI 矩阵而悄悄撤回。确有资源受限而无法保留旧 engine ABI 时，按公开兼容变化处理并提供迁移途径；不伪造“新版必然兼容旧版”。

## 8. 源码、产物和发布职责

`pxa-system` 管理唯一发行号、Core/Service 规范、Guest SDK、libpxa、工具链配方、模拟器核心和测试。产品工作区管理板驱动/固件及其锁定的系统提交；PXADB/屏幕 profiles 当前在产品侧的通用部分应整理成独立安装资源。可以由产品仓库拼装 DevKit，但官方分发清单必须列出全部来源，不能留下 App 用户对产品源码树的依赖。

`pxa-apps` 的示例/游戏独立发行；各项目固定经过测试的 SDK lock 和最低 Host profile。无需因为 Host 发行新 PATCH 而全部 App 升版；AOT 重打包形成新包时提升 App 的 `release_sequence` 并保留同一发布者身份。

源码继续公开 tag/归档、固定 Git 子模块和依赖提交，供维护者重建。不把源码归档当作普通用户的 SDK 下载。开发套件采用“源码生成并测试一次、依赖摘要固定、分平台打包”的工作流，无需为了发布再复制一份运行时实现。Guest C++ 源码/生成器能随 SDK 分发；Host/WAMR/LVGL 不需要成为每个 App 工程的源码依赖。

## 9. 发布流水线及验收门槛

1. 发布 PR 更新唯一版本、变更日志、API/ABI/引擎变更说明、支持平台与兼容矩阵。禁止正式发布脏树、浮动依赖或版本/生成物不一致的内容。
2. 从候选 tag 和固定系统/产品提交，在固定宿主构建环境生成 SDK、CLI、工具链、模拟器和需要发布的板型固件；为每个平台记录动态依赖、最低 OS、完整来源和许可证/SBOM。
3. 执行协议 golden/malformed、C/C++ SDK、Host、资源/签包/安装、WASI/AOT、工具回归；保留并运行旧正式二进制包，测试新 SDK→旧基线以及 AOT 不兼容诊断。
4. 进行空白环境验收：移除所有源码树可见性，不允许访问构建者绝对路径；只用发布资产编译/运行 C Hello、C++ 声明 UI、资源/音频、IPC/Work/Surface 和一个真实游戏。验证离线、只读安装目录、路径含空格、搬移、多版本和锁定缓存失效。
5. 模拟器运行输入、截图、包更新/取消/错误、前后台、权限/存档和系统层 UI 检查；屏幕/DPI profile 与能力报告一致。模拟器是功能运行器，不是 ESP 性能预测器。
6. 已承诺板型完成真机安装/启动/更新/重启/恢复/音频/文件/低预算冒烟；基准记录实际显示 FPS、阶段耗时、稳定占用及本次峰值，区分首次初始化与历史峰值。没有本轮真机证据的板型只标交叉构建可用，不能标完整认证。
7. 正式 tag 重建并复验，生成 `distribution.json`、资产摘要、构建证明、SBOM、release notes 和迁移/EOL 说明。候选版与正式版元数据不同，不能直接重命名 RC 二进制冒充正式版。
8. 先准备完整草稿发布、附所有资产、再公开；公开后冻结 tag/资产。GitHub 的 [不可变发布流程](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)可用于此目的。latest/stable/preview 只是更新通道，项目最终仍锁确切版本和摘要。

可在新版本出来后撤下通道推荐、标记某版已撤回及原因，但不覆盖旧摘要对应资产。CLI 阻止新项目误选已撤回版本，并允许既有项目按明确策略复现。固件升级前通过当前装载规则检查已安装应用；失败保留应用/数据。固件回滚还需兼容持久状态和 App 新存档，不能只看二进制能否运行。

## 10. 分阶段实施与完成标准

| 阶段 | 主要改动 | 验收结果 |
| --- | --- | --- |
| R0：版本和契约 | VERSION 解析/生成、规范 v1 状态整理、独立 Host C ABI、版本一致性检查、设备能力 JSON | 所有工具与设备明确报告发行与 ABI；错误不混淆板固件版本 |
| R1：真正的独立组件 | 合并 C/C++ Guest 发布目录，现有 packager 接受 App 路径，工具依赖封装，模拟器/字体/profiles 可重定位 | 在无源码目录用现有命令完成构建、签包、模拟运行；已有外部消费成果作为基线 |
| R2：用户入口 | CLI init/build/run/check/device、项目 lock、离线/在线 DevKit、多版本、缓存键、签名身份管理 | 一份 DevKit 完成 Hello 到真机部署，构建不依赖 ESP-IDF或临时编译 wamrc |
| R3：兼容保障 | capability profiles、introduced-in 元数据、别名迁移、诊断、旧包封存矩阵、AOT 更新与预算门禁 | 升级 SDK 不无意抬高 profile；支持/拒绝/回退均有可重复测试 |
| R4：正式发行 | 各宿主平台 CI、空白消费、板型实机、不可变发行资产和支持策略 | 对外发布已通过矩阵的平台；下载资产可复现，保留旧版本及迁移证据 |

实施顺序先 R0/R1，随后打通最小 R2，再扩展 R3/R4；不等待复杂自动更新中心或包仓库才能发布第一套可用工具。首版 DevKit 不引入 App 运行时的新默认缓冲、缓存、任务池或解释器；版本管理主要增加构建/分发侧工作及少量只读元数据。

### 10.1 首个候选版的实现范围

`0.2.0-rc.1` 已实现 R0、R1 和最小 R2：统一发行元数据、独立 Host C ABI 0、实际启用能力查询、可重定位的 C/C++ SDK 与 Linux x86_64 DevKit、项目 lock、用户签名身份、离线安装及跨目标打包。Guest Core 仍为 1.0，SDK 发行号与 Host C ABI、固件版本和 App 版本分别管理。C/C++ 模板均默认 O3。

R3 已实现显式 Core/服务范围冻结、字段别名冲突检查、签名后兼容诊断、实际目标/AOT ABI 选择，以及 SDK 更新保留最低运行要求。introduced-in 的逐命令编译检查、完整历史版本矩阵及所有预算的离线预测尚未实现；Host 激活检查仍是最终依据。新 SDK 的源码兼容性遵循 SDK 发行版本，已签名旧包则按 Core、服务与 engine ABI 判断。

首版 R4 的支持范围限定为 Linux x86_64、glibc 2.35 及以上。模拟器和工具链必须经过基准环境及仓库外消费验证后才发布，不把交叉构建等同于板型完整认证。Windows、macOS 和 Linux ARM64 留作后续发行平台。精确依赖、构建与验收方法见系统仓库 `docs/BUILDING_RELEASE.zh-CN.md`、`docs/DEVKIT.zh-CN.md` 和 `docs/VERSIONING.zh-CN.md`；实际资产的 `distribution.json`、SBOM、SHA256 和发布验收报告共同记录最终证据。

正式交付的核心标准是：开发者在全新环境中仅拥有模拟器、SDK 和开发套件便能生成并运行 `.pxa`；设备独立验证真实兼容性；相同冻结输入的归档可以复现；换 SDK、更新固件或重打包 AOT 的影响都有明确说明和测试证据。

## 审计依据

- `deps/pxa-system/VERSION`、system/libpxa CMake、`libpxa/include/pxa/version.h`、`idf_component.yml`。
- `scripts/package-sdk.sh`、`tools/package/build_guest_cpp_sdk.sh`、`resolve_wasi_sdk.sh`、`package_app.sh`、`build_package_manifest.py`、`build_pxa_provenance.py`。
- `sdk/cmake/PxaGuest.cmake`、`sdk/guest-cpp/README.zh-CN.md`、`VALIDATION.zh-CN.md`。
- `spec/draft/abi-1.0-envelope.json`、`pxa-core.json`、各服务 JSON、`package.md`、Package/Container JSON。
- `libpxa/src/package/inventory.c`、`artifact_select.c`、`manifest_decode.c`、`config/wamr.json`。
- `simulator/desktop/CMakeLists.txt`、产品工作区 `tools/simulator.sh`、`firmware/components/pxadb/src_pxa_service.cc`、PXADB Python 工具。

外部设计依据为 [SemVer](https://semver.org/spec/v2.0.0.html)、[WASI SDK 官方用法](https://github.com/WebAssembly/wasi-sdk/blob/main/README.md)、[WAMR AOT 兼容说明](https://github.com/wasm-micro-runtime/wasm-micro-runtime/blob/main/doc/build_wasm_app.md#aot-compiled-module-compatibility-among-wamr-versions)、[CMake 预构建包集成](https://cmake.org/cmake/help/latest/guide/using-dependencies/index.html)及 [GitHub 不可变发布](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)。本方案依据当前本地源码与既有验证记录，不把外部上游支持列表当作 PXA 实测认证。
