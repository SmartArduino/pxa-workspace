# 游戏固定 SD 资源、图标与中文名称验收（2026-10-21）

本轮将五个 recrof 移植游戏统一为单一正式应用，并将 OpenLara 改为仅从 Host 媒体根读取游戏数据。六个应用均已生成 ESP32-S31 AOT 安装包，名称不再含 `-sd`。本轮没有达成所有游戏 20 FPS 的目标。

应用仓库提交：`aad3417b168e83fc7dcdde2bc8978bf85f4bdf9a`；SDK 安装器修复提交：`7877619`。原始日志和构建产物不纳入这些提交。

## 应用和安装包

| App ID | 版本 | 简体中文名称 | 容器字节 | 包内文件字节 |
| --- | --- | --- | ---: | ---: |
| pxa-quake | 0.5.0 | 雷神之锤 | 1,526,769 | 3,043,564 |
| pxa-quake2 | 0.2.0 | 雷神之锤 II | 3,239,154 | 4,506,257 |
| pxa-duke3d | 0.2.0 | 毁灭公爵 3D | 1,688,600 | 2,381,702 |
| pxa-command-conquer | 0.2.0 | 命令与征服 | 3,969,970 | 5,503,152 |
| pxa-red-alert | 0.2.0 | 红色警戒 | 5,133,082 | 7,241,223 |
| pxa-openlara | 0.6.0 | 古墓丽影 | 1,894,387 | 2,737,887 |

安装包位于工作区 `build/game-unify-20261021/packages/`。`package-audit.json` 核对六个签名清单的文件长度、SHA-256、图标路径、版本及中文 metadata；包内不含 PAK、GRP、MIX、PHD、AD4 等游戏媒体。包内文件总量不包含文件系统和安装暂存开销。

原 `*-sd` 包装目录已移除。正式 CMake 保留原 SD 配置，Duke 使用已在设备运行的 native main-loop、原始 PCM 和借用索引帧缓冲路径。构建会检查执行模型与包声明一致，并拒绝把游戏数据放回 `assets/game`。对比配置显式声明开关，避免继承正式包的新默认值。

## 固定目录与迁移

媒体位于选定卷的 `PXA/media/<PublisherDir>/<AppId>/`，例如 `/PXA/media/lucinhu/pxa-openlara/`。部署脚本通过 `PACKAGE paths <publisher>:<app>` 获取 Host 实际绑定，用户无需选择目录；不可用时停止，不伪造 Flash 媒体根。App ID、签名身份和存储目录不会随显示语言变化。本次设备的媒体卷是 SD，应用代码仍安装在 Flash。

新增 `PACKAGE move-data` 只允许同一发布者的应用私有 `data/` 迁移，不复制授权文件。运行时通过所有者队列执行，持有事务锁和元数据锁；磁盘意图记录与同卷重命名支持重试。目标须已安装，应用运行时拒绝迁移。部署工具默认只读计划，实际迁移要求本地备份，核对文件 SHA-256，遇冲突停止。

Quake 原 `state.nvm`（2,064 B）迁移前后 SHA-256 均为 `e4823347f2176d3e2437e27c1ce190d786b3875225dc79ded4ef5dda08d48e5b`。Duke 的 18 个私有数据文件逐一核对，旧 `external-data.grant` 未迁移。游戏运行时的迁移请求返回忙碌错误，前台应用继续运行。

OpenLara 本地资源暂存目录为 `local/pxa-apps/.game-data/openlara/files/`，各目标的安装包都只包含引擎和桌面资产。Guest 仅通过 FS 0.3 的媒体根和 scoped open 读取资源，移除包内媒体回退；音乐保留 64 KiB FIFO 和 8 KiB 读取上限，处理短读、暂时忙碌和取消。旧 AppKey 目录里与准备清单完全一致的资源采用同卷重命名，目录索引最后发布。当前仅有公开演示版 City，不能据此宣称完整原版全关卡验收。

## 图标与 i18n

六个图标均为 96×96 RGBA PNG。前五个依据各自原版标志重绘；OpenLara 使用独立的经典低多边形劳拉、青绿色背心、双枪和古墓石门画面，未采用金属徽章候选。它们是内置 imagegen 生成的重绘资产，不标注为未经修改的官方原图。源链接、完整提示词、文件哈希与许可证记录在 `local/pxa-apps/common/recrof/icons.json` 和 `ICONS.md`。

每个应用通过 `i18n/messages.yaml` 及 metadata catalog 提供 `zh`、`zh-CN`、`zh-Hans`、`zh-TW`、`zh-Hant` 名称，其他语言回退英文。利用现有 SDK 清单国际化，不给 Guest 添加翻译表或常驻任务。真机中文桌面已验证“古墓丽影”“雷神之锤”“毁灭公爵 3D”与新版图标。

## 真机结果

设备为 DOIT GameKit ESP32-S31，MCU `1c:29:04:d0:af:5e`，320 MHz、16 MiB PSRAM、320×240 显示。测试固件 ELF SHA-256：`2017e43a09d93ee2f932d664ae4e8346866686dd35574192a12749b23f956942`。测试期间未改分区表或清除用户数据。

| 游戏 | 场景与采样 | PERF FPS 均值 | LCD 完成帧率 | 结果 |
| --- | --- | ---: | ---: | --- |
| Quake | 实际关卡，60 次 | 9.17 | 9.24 | 原生音频、手柄短按、方向、触屏/手柄浮层切换通过 |
| Duke Nukem 3D | 实际关卡，60 次 | 20.70 | 20.79 | 原生音频、手柄、浮层与断开恢复通过 |
| OpenLara | City 固定位置，320×240，30 次 | 9.27 | 未单独计算 | 转向、前进、存档恢复、背包、20 次 120 ms 短按通过 |

OpenLara 本次没有执行硬件重启恢复测试。Quake 和 OpenLara 均未达到 15/20 FPS；Duke 的结果适用于本次测量场景，不代表所有地图保持 20 FPS。Quake II、Command & Conquer、Red Alert 本轮完成构建和包审计，未进行新一轮真机性能验收。

OpenLara 测试前 Flash 仅剩 2,478,080 B，不能与现有游戏同时容纳其引擎。测试采用临时归档 Quake 代码、保留私有数据及 SD 媒体的方式。验收后归档 OpenLara 代码并恢复 Quake、Duke 的最新版图标/i18n 包；OpenLara SD 媒体与新存档保留。GB/GBC 与 NES 保持原安装。

## 安装器空目录边界修复

归档最后一个应用后，发布者目录不存在时，安装器的 `open_tree_at(..., create=0)` 将 ENOENT 错报为 INTERNAL，导致已完成代码删除的归档返回失败，后续安装恢复也被阻止。现在准确返回 NOT_FOUND，由已有目录同步逻辑处理。新增回归测试在卸载后移除空发布者目录，再次卸载：修复前稳定失败（-11），修复后完整 adapter 测试通过。

设备安装器修复沿用原验收固件的其他编译对象，只重编译 `pxa_posix_installer.c` 并重新链接；对所有对象的 SHA-256 比较确认仅此一个对象变化。最终 ELF 为 `79252868b89cf4e2004a33246b9b7e48561c8d0c30cd386f6a9dfb714af738a0`，只更新应用分区，原固件保留为回滚镜像。上述 FPS 表仍对应原验收固件，不冒充新固件重测结果。构建与回滚记录位于 `installer-fix-firmware/manifest.json`。

最终固件上已完成 Quake、Duke 的签名容器部署、清单/图标/AOT 哈希核对、启动与退出检查。OpenLara 留在 SD 的 7 个媒体文件全部重新核对通过。设备最终回到桌面，没有活动游戏；可用 Flash 为 2,441,216 B。

## 自动检查及证据

- 通用导入、部署、迁移 Python 测试：31 项通过；PXADB 测试：94 项通过。
- Host 适配层测试通过，私有数据迁移和主循环测试包含 sanitizer 检查。
- OpenLara 媒体规划测试：5 项通过；实际 WASM 缺卡、缺资源、短读和 WOULD_BLOCK 测试通过，禁止包内媒体回退。
- OpenLara 实际 WASM 集成测试覆盖跨实例存档、失败替换保留旧存档、损坏记录、音乐与 20 次关卡切换；目录扫描覆盖大小写和无效/超大文件。
- 六个最终清单的 i18n 名称和签名文件库存核对通过；源码 `git diff --check` 通过。

原始日志、截图、固件快照及回滚镜像均留在忽略目录 `build/game-unify-20261021/`，不作为源码提交。主要证据为 `quake-device-final/measurement.json`、`duke-device-final/measurement.json`、`openlara-device-final/metrics.json`、`package-audit.json`、`openlara-catalog.json`、`openlara-sd-tests.json`、`restore-games-i18n.json` 和 `final-smoke.json`。
