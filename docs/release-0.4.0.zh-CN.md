# PXA 0.4.0 发布与验证

2026-10-16 至 17 日完成本轮发布。Core 保持 2.0，AOT ABI 保持 `wamr-pxa-aot-v6-core-1`。SDK、工作区、应用和服务端分别按功能提交，发布 tag 均为 `v0.4.0`；已有发布和用户数据保留。

## 发行入口

- [SDK / 工具链 / 完整 DevKit / 容器 / 宿主机工具](https://github.com/SmartArduino/pxa-system/releases/tag/v0.4.0)：14 个附件，清单及签名通过校验。
- [五板完整固件、应用分区与刷写说明](https://github.com/SmartArduino/pxa-workspace/releases/tag/v0.4.0)：每板 merged.bin、app.bin 和 ZIP，另有签名总清单、公钥及 SHA256SUMS。
- [全部 22 个应用的四目标离线集合](https://github.com/SmartArduino/pxa-apps/releases/tag/v0.4.0)。商城已更新 88 个签名包及 66 张实际截图；[详细应用报告](https://github.com/SmartArduino/pxa-apps/blob/v0.4.0/docs/store/20261016-release/REPORT.zh-CN.md) 也随应用仓库分发。
- [商城与官网服务端](https://github.com/SmartArduino/pxa_app_store_backend/releases/tag/v0.4.0)，已部署至 [app.doit.am](https://app.doit.am) 和 [pxa.doit.am](https://pxa.doit.am)。发行说明中文在前、英文在后。

## 本轮验证

| 项目 | 结果与边界 |
| --- | --- |
| DevKit / SDK | 固定 Ubuntu 构建环境的 114 项模拟器与 SDK 测试全部通过 |
| 外部工程 | 从正式归档解压到含空格路径，离线构建 S3、S31、桌面目标并运行 Hello |
| 在线安装 | 从公网镜像验证发行签名、五组件哈希，原子安装 0.4.0；使用安装后的 SDK 离线构建成功 |
| 跨平台方案 | 容器断网构建三种目标并运行模拟器；导出的宿主机工具和签名清单经过验证。Windows/macOS 原生工具链不包含在本版本，需按教程采用容器/WSL 与宿主机串口；未在这些系统上进行原生实测 |
| 应用 | 22 × 4 目标全部构建；应用原生回归（含 Sanitizers）通过；公网完整下载 88 包，对比版本/序号、字节数、Content-Length 与 SHA-256 |
| 图片与网站 | 每应用 3 图，共 66 图可解码且网页引用正确；25 个部署 HTML 与本地构建逐字节一致，22 篇教程的链接/资源/锚点/搜索检查通过 |
| 后端 | 33 项测试通过；原生 AOT 优先与 Wasm 回退经过回归 |
| Host / 资源 | ESP Host、板级、输入、显示覆盖层及完整资源流水线测试通过；包含低预算、存储停顿、音频、前后台与加载中退出 |
| 固件 | 五板均编译、校验内置版本 0.4.0；Flash 分区范围、合并区域哈希、镜像校验和、ZIP CRC 通过；挂载读回工厂 LittleFS，逐文件对比字体与同一份签名商城包 |
| 真机 | 本轮发布流程未重新烧录设备、未重新测量游戏 FPS/触摸或声音延迟，不把构建和模拟器结果写成真机性能收益 |

## 五板固件

| 板型 | 芯片 | 完整镜像 | 应用分区文件 |
| --- | --- | --- | --- |
| pai-touch | esp32s3 | 16 MiB | 3,509,296 B |
| esp-mosaico | esp32s31 | 16 MiB | 4,101,456 B |
| esp32s31-korvo-1 | esp32s31 | 16 MiB | 3,923,232 B |
| sensecap-watcher | esp32s3 | 32 MiB | 3,623,600 B |
| doit-gamekit | esp32s31 | 16 MiB | 3,898,208 B |

完整合并固件包含 bootloader、分区表、系统应用和工厂 LittleFS（简体中文字体及商城 0.8.14）。字体使用同一份大陆字形黑体子集；商城直接复用本轮发布的原生包，不另行重建签名。

**merged.bin 用于首次部署，会覆盖已有应用、存档和设置。需要保留数据时，仅在分区表一致的前提下烧录 app.bin；存储区的字体和应用也会保留原有版本，应单独更新。** S31 的说明保留 `--no-stub`，手动进入下载模式时使用 `--before no-reset`。本轮没有执行这些烧写操作。

## 发布中发现并修复的问题

1. 新 Host 能力快照查询可能从同一应用版本选到 Wasm，而遗漏兼容的原生 AOT。现先匹配芯片/引擎/ABI 的原生包，再回退 Wasm，并保持每个组件和服务能力检查。
2. 固件版本曾取到独立的 `host-tools-v0.1.0` Git 标签。现直接使用 SDK VERSION，五板在最终镜像中均核验为 0.4.0。
3. Watcher 的旧依赖依赖 IDF 过往的隐式包含关系。通过板内 CMake 连接拆分的 GPIO/I2C/I2S/SPI、版本与堆头文件；保留原有依赖版本，不修改下载缓存，也不增加设备运行时分配。
4. 独立构建目录仍共享 managed_components；切回旧板型可能引用已被另一板替换的文件。构建工具在板型切换时、持有全局锁后刷新依赖图，同板热构建保留原路径。假 IDF 测试验证切回、配置失败重试和 monitor 不触发配置；实际 Doit GameKit 的旧缓存重建通过。
5. 补齐工作区缺失的公开 SDK 子模块映射，实际验证 SDK、WAMR 和 page-manager 的固定提交检出。
6. 更新过时的 C 测试路径/Core 断言，以及商城图片测试所需的签名输入法环境；完整资源流水线最终全部通过。USB 路由测试在临时工作区运行，不再改变真实构建状态。

这些构建修复没有引入默认大缓存或新的设备常驻分配。原有性能记录继续保留；本轮不据此给出新的 FPS 或内存峰值数字。

## 复现

```sh
git clone --branch v0.4.0 --recurse-submodules https://github.com/SmartArduino/pxa-workspace.git
cd pxa-workspace
git clone --branch v0.4.0 https://github.com/SmartArduino/pxa-apps.git local/pxa-apps
# 激活匹配目标的 ESP-IDF 后；S31 需要相应的新 IDF 工具链：
for board in pai-touch esp-mosaico esp32s31-korvo-1 sensecap-watcher doit-gamekit; do
  PXA_FIRMWARE_BUILD_ROOT="$PWD/local/clean-firmware" tools/firmware.sh "$board" build
done
# 使用开发者自己的签名身份重新生成 Store；该重建包的哈希不等于本次官方包。
# 建议先设置 WAMRC / WASI_SDK_DIR，复用正式 DevKit 中的工具链。
tools/app.sh build store --target esp32s3 --aot-only --output local/release-packages/esp32s3
tools/app.sh build store --target esp32s31 --aot-only --output local/release-packages/esp32s31
# 为工厂数据使用同一份构建结果中的 .pxa 和同名展开目录：
python tools/build_firmware_release.py --output local/firmware-release \
  --packages local/release-packages --build-root local/clean-firmware --skip-build \
  --idf-python "$IDF_PYTHON_ENV_PATH/bin/python"
```

合并脚本还需要 `littlefs-python`（本轮使用 0.15.0；`--littlefs-python` 可指定）。`--packages` 必须指向同时包含容器与同名展开目录的构建输出；GitHub 应用离线合集只包含容器，不能直接代替此输入。生成目录中的 ZIP 自带板型、芯片、分区表、双语操作说明与哈希。DevKit 的独立安装、容器构建与宿主机调试见官网教程；它们不要求获取固件源码或在用户电脑重新编译 LLVM。

DevKit 的构建输入分别记录为 SDK `9cb5e6643be41e873154c4690eba3baf88b181cf`、工作区 `4e05c13a4078eb2696d9f9e762f4d48bb6173b7a`；固件来源提交单独记录在总清单。随后的工作区提交包含本轮构建兼容修复和验证报告。报告里的结果均来自本次对应步骤，签名归档不含发布私钥。
