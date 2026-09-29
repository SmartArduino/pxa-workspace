# PXA 版本迭代与独立 App SDK 发布方案

状态：设计方案，尚未实现发布流水线。示例版本 `0.2.0` 用于说明规则，不代表该版本已经发布。

## 现状与目标

当前 `pxa-system` 与 `libpxa` 的 CMake 版本均写为 `0.1.0`，`scripts/package-sdk.sh` 也默认使用 `0.1.0`，但它安装的是 Host C 库与 CMake 元数据，并不包含独立开发 App 所需的完整 Guest SDK、打包 CLI、预编译 `wamrc` 和模拟器。Guest SDK 没有自己的发布版本来源。现有 `package_app.sh` 假设自己位于完整 `pxa-system` 源码树内，缺少 `wamrc` 时还要从 WAMR/LLVM 源码构建。桌面模拟器也引用产品仓库中的字体和配置。因此，当前不能把已有 SDK tar 包直接交给只开发 App 的用户。

发布后，App 开发者只需下载版本固定的开发包，不需要克隆 `pxa-workspace`、`pxa-system`、WAMR、LLVM 或 ESP-IDF。固件开发者仍使用完整源码。发布产物必须能在空白环境中通过下载、校验、构建 Hello App、运行模拟器完成验收。

## 一套发布号，几套独立兼容号

| 名称 | 示例 | 用途和比较规则 |
| --- | --- | --- |
| PXA 发布版本 | `0.2.0` | SDK、CLI、工具链分发包、模拟器、对应固件发布使用同一个版本号。一个正式发布只对应一组不可变产物。 |
| App 版本 | `0.1.0`、`0.1.1` | 每个 App 独立迭代；与 PXA 发布版本无须一致。安装更新仍使用 App 的发行序号与发布者身份。 |
| Core ABI | 当前 `1.0` | Guest 与 Host 的导入、事件信封、句柄语义。Host 按 ABI major/minor 判断兼容，不要求 App 的编译 SDK 发布号与固件完全相同。 |
| Service ABI | 例如 UI、Audio、Assets 各自的 major/minor 和 feature bits | 每项服务独立协商；App 清单声明需要的版本范围与特性。 |
| AOT engine ABI | 当前 `wamr-pxa-aot-v6-core-1` | AOT Artifact 与设备必须精确匹配目标架构和 engine ABI；更换 PXA 发布号本身不自动改变此值。 |
| Package 格式与 Host C ABI | 各自独立 | 前者决定安装器能否解析包；后者决定原生程序是否能链接 `libpxa`，不作为 Guest SDK 版本。 |

上述发布号采用 [SemVer](https://semver.org/) 的 `MAJOR.MINOR.PATCH` 与 `-rc.N` 格式。现阶段 `0.x`：兼容修复升 PATCH；增加公开能力或破坏实验性接口升 MINOR；达到稳定契约后发布 `1.0.0`，此后不兼容变更升 MAJOR。即使只修改固件或模拟器，每次正式 PXA 发布也升号，并重新生成同版本的 SDK/CLI/模拟器元数据，不产生“SDK 0.2.0 对应 PXA 0.2.1”的组合。若此前 `0.1.0` 仅是源码占位而没有对外发布，可以直接以 `0.2.0` 作为首个正式候选版；是否叫 `0.1.0` 由首次公开发布时确定，不能靠当前 CMake 字符串推断。

Core ABI 或 Service ABI 变化时，分别更新其规范和 golden vectors。新增能力应增加 Service minor 或 feature bit；破坏现有语义时增加对应 ABI major，并给出 App 迁移说明。WAMR 或编译器更新只有在 AOT 装载兼容性变化时才增加 `engine_abi`；这种发布至少按 PXA MINOR 处理，不能当作普通 PATCH，因为已安装的 AOT 可能需要重新构建。固件应在保留旧 AOT 装载能力或提供 Wasm 回退的情况下升级，否则发布说明必须明确指出重打包要求。

**把当前 `min_sdk`、`target_sdk`、`compile_sdk` 从“SDK 发布版本”概念中拆出来。** 这三个 `[1,0]` 字段现在表达 Core ABI，而不是 `0.2.0` 这样的下载版本。下一次修改 App 源清单格式时，建议改成 `min_core`、`target_core`；`compile_core` 和 `built_with_pxa_release` 由打包工具自动写入构建来源记录。二进制清单中现有 Core 要求字段可继续按原语义编码，无须仅为了改名而破坏已安装包。`target_core` 是行为策略版本，不是运行时上限；`built_with_pxa_release` 只用于诊断和复现，不做启动时的精确版本门禁。

例如 App 使用 PXA SDK `0.2.0` 编译、要求 Core `1.0` 和 Assets `2.1`，可以在满足这些要求的 PXA `0.3.0` 固件上运行。若 AOT engine ABI 不匹配，只有包内存在 Wasm 且该固件启用了解释器时才能回退；否则安装或激活时给出明确的不兼容原因。App 不应为了升级编译 SDK 而无意义地提高 `min_core` 或 Service 最低版本。

## 版本的唯一来源

以 `pxa-system/VERSION` 为唯一手工维护的 PXA 发布号。构建时由它生成 CMake `PROJECT_VERSION`、Guest `pxa_version.h`、CLI 与模拟器 `--version`、固件报告的 `pxa_release`、产物文件名及发布清单。`libpxa` 包版本同 PXA 发布号；其原生 `SOVERSION` 由独立的 Host C ABI 策略决定，不能简单等同于发布号的 major。现有 `pxa-system/CMakeLists.txt`、`libpxa/CMakeLists.txt` 和 `scripts/package-sdk.sh` 的三个手写 `0.1.0` 应移除。

规范 JSON 是 Core/Service/Package 版本的来源；`config/wamr.json` 是 WAMR、LLVM 和 engine ABI 的来源。发布 CI 生成只读 `distribution.json`，记录 PXA 发布号、Core/Service 能力、Package 格式、engine ABI、工具链提交与 SHA-256、支持的宿主机平台及各板型固件提交。CI 拒绝版本、tag、产物内元数据不一致的构建。App 仓库提交 `pxa.lock`，固定 PXA 发布号和 `distribution.json` 摘要，避免开发者之间隐式使用不同编译器。

## 发布产物

第一阶段只承诺当前可验证的 **Linux x86_64 开发宿主机**；同一个 `wamrc` 可为当前 ESP32-S3、ESP32-S31 和 Linux 模拟器输出不同目标 AOT。Linux arm64、macOS、Windows 要在各自完整构建与空白环境验收通过后再列入发布矩阵，不根据上游工具“理论支持”宣称可用。

| 产物 | 内容 | 使用者 |
| --- | --- | --- |
| `pxa-app-sdk-0.2.0-any.tar.zst` | Guest C 头文件、CMake 模块、导入白名单、App 模板、API 文档及版本元数据。无需 Host C 库。 | 所有 App 开发者 |
| `pxa-cli-0.2.0-linux-x86_64.tar.zst` | `pxa` 入口、Manifest/资源/i18n 编译与签包工具、Wasm 导入校验、PXADB、独立运行所需脚本。 | 所有 App 开发者 |
| `pxa-toolchain-0.2.0-linux-x86_64.tar.zst` | 经 PXA 验证的 wasm32/WASI 编译器、sysroot、`wamrc` 和目标参数。内部记录原始 wasi-sdk、WAMR、LLVM 版本与摘要。 | 本机编译与 AOT 打包 |
| `pxa-simulator-0.2.0-linux-x86_64.tar.zst` | 可重定位的 SDL/LVGL 模拟器、字体/显示配置、运行库和 PXADB 本地服务。 | 无 ESP 设备时运行 App |
| `pxa-host-sdk-0.2.0-linux-x86_64.tar.zst` | `libpxa` 头文件、库和 CMake 导出。现有 `package-sdk.sh` 产物应改名归入此类。 | 固件/Host 集成者，普通 App 不需要 |
| `pxa-firmware-0.2.0-<board>.*` | 板型固件、分区布局、刷写说明与对应 `distribution.json`。由产品工作区发布。 | 设备用户与板级开发者 |

每个发布页另附 `distribution.json`、`SHA256SUMS`、许可证清单、SBOM、版本变更与迁移说明。工具链体积大，可把 CLI、SDK、模拟器分开下载；`pxa doctor` 从分发清单按摘要获取缺失组件并缓存。官方 App 开发入门只下载前四项，不下载源码或 Host SDK。发布文件中不包含 App 发布者私钥；本地开发密钥由 CLI 在用户目录生成，正式签名密钥由开发者或其 CI 单独保管。

## App-only 使用路径（目标命令，当前尚未实现）

```sh
pxa sdk install 0.2.0                 # 验证 distribution.json 和归档摘要，下载所需组件
pxa init hello --sdk 0.2.0             # 生成 package.json、src/main.c、pxa.lock
cd hello
pxa build --target simulator           # 输出 dist/hello.pxa 和构建来源记录
pxa run --sim --board pai-touch        # 使用对应显示/输入配置；不是 ESP 速度仿真
pxa build --target esp32s3,esp32s31   # 生成一个多目标包，按需包含 Wasm 回退
pxa device install dist/hello.pxa --port /dev/ttyACM0
```

`pxa init` 的 `package.json` 只写 App 身份、App 版本、所需服务/权限、源文件和资源；默认 Core 要求来自 SDK 模板，不要求开发者手工填写工具链细节。`pxa.lock` 与 App 源码一起提交。`pxa build` 使用项目锁定的工具链，构建到临时目录，签名与清单验证成功后原子替换 `dist/` 产物，并输出精确的 SDK、编译器、engine ABI、源码与包摘要。`pxa doctor --device` 通过 PXADB 读取设备 PXA 发布号、Core/Service 能力、目标架构、engine ABI 与 Package 格式，在部署前解释不兼容项。

现有 `package_app.sh` 与 `tools/app.sh` 可以作为第一阶段实现的内部构件，但要改为接受**任意独立 App 目录和输出目录**，通过安装前缀寻找 SDK/工具链，移除对 `pxa-system/apps/pxa`、源码树相对路径、WAMR Git 子模块、在用户机器编译 LLVM 及产品工作区字体的依赖。现有 CMake App 仍可使用已发布的 `PxaGuest.cmake`；直接 C App 不要求 CMake。模拟器应从可执行文件附近定位资源和配置，不能在二进制中烘焙当前产品源码路径。

## 仓库与发布职责

- `pxa-system` 是 PXA 发布号、Guest SDK、CLI、工具链配方、模拟器、ABI 规范和分发清单的唯一发布源。建议在其公开发布站点提供 `vX.Y.Z` tag 与下载资产；仓库源码归档只是额外选项，不是 App 开发者的安装路径。
- `pxa-workspace` 固定一个 `pxa-system` 提交，构建板型固件并发布与该 PXA 版本对应的固件资产。产品专用板驱动可有自己的构建修订，但对外公布的 PXA 版本仍取自锁定的系统版本。
- `pxa-apps` 里的示例和产品 App 各自发布 App 版本，CI 用 `pxa.lock` 固定所测试的 PXA 发布版；App 仓库不驱动 PXA 全局升版。

内部试验可使用带提交摘要的快照构建，但不能标成正式 `X.Y.Z`。正式版本使用不可移动的 tag；发布失败或发现缺陷时修复并升 PATCH，不覆写旧资产。若选择 GitHub Releases，按其[不可变发布建议](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)先建立草稿、上传和验收所有资产，再公开发布。

## 发布流水线与准入条件

1. 合并发布 PR：修改唯一 `VERSION`、变更日志和兼容矩阵；若 ABI 或 engine ABI 变化，同时提交规范、golden vectors、迁移说明和旧 App 样本测试。发布候选版使用 `vX.Y.Z-rc.N`。
2. 从干净的 `pxa-system` tag、固定的依赖提交和工具链镜像构建所有分发包；将确切的系统、产品工作区、App 测试仓库提交写入 `distribution.json`。生成 SHA-256、SBOM 和许可证清单。
3. 跑 Core/Service/Package golden 与 malformed 测试、Guest C/C++ 头文件测试、Host/WAMR 测试、签包/安装/更新测试、全部目标 AOT 构建和各板型固件编译。用上一正式 SDK 构建的样本包验证新 Host 的兼容性，并验证不兼容 AOT 得到明确诊断。
4. 在不含三个源码仓库、WAMR/LLVM/ESP-IDF 的空白 Linux x86_64 环境中，仅下载发布资产：运行上文 Hello 工作流、模拟器 UI/输入/音频/资源测试以及 PXADB 本地安装；验证 SDK/CLI/模拟器/固件元数据的 PXA 版本一致。
5. 候选版在 Pai Touch 和 Korvo 实机上完成安装、启动、后台/熄屏/恢复、音频与资源读取冒烟测试；记录设备固件、App 包、engine ABI 和结果。硬件验收失败不能升级正式发布。
6. 创建最终 `vX.Y.Z` tag，重建并复验正式资产，先上传到草稿发布页，再公开发布。发布后保持 tag 和资产不可变；问题通过下一 PATCH 版修复。若资产平台支持，附构建来源证明；至少保证下载时的摘要校验。

第一次实施的完成标准是：开发者在新目录中只用发布资产构建并运行 Hello 和一个真实资源 App；`pxa --version`、模拟器 `--version`、固件设备信息都给出同一 PXA 发布号；设备端仍依据 Core/Service/engine ABI 判断 App 兼容；现有源码仓库构建路径继续通过回归。按实现顺序，先统一版本来源和重命名 Host SDK 包，再使 packager/模拟器可重定位，随后提供预编译工具链与 CLI，最后接上发布 CI、空白环境验收和实机门禁。
