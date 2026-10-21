# PXA 作者、完整应用身份与短目录实现验收

日期：2026-10-21。用户要求实际实现，允许重构；未制作方案展示页面。代码已修改在固件仓库、其 pxa-system 依赖、NES/Store 应用以及 `/home/lucinhu/Documents/workspace/python/app_store_backend`。接入说明见 [实现文档](../pxa-identity-implementation.zh-CN.md)。

## 实现结果

- 应用身份保持 `(已验证签名谱系根, app_id)`；Host 和商店启动、卸载、查询更新不选择同名第一项。
- 包源 `publisher` 生成签名清单内的作者描述；离线作者按签名根隔离，可信在线作者按产品固定 issuer 与 UUID 隔离。同名离线目录增加有界 Base32 指纹，商店重复 handle 返回 409。
- 受保护 NVS 注册表把可读目录绑定到完整身份。POSIX 安装器增加兼容旧 config 尺寸的可选回调；事务、锁和 owner 控制记录保留完整身份。
- 包、私有数据及 SD 媒体根接入同一映射。迁移使用持久意图、非覆盖 rename、完成提交；不复制或重建用户文件。SD 未就绪可运行 Flash 应用，访问 SD 前重试恢复。
- Store Installer 0.7 新增操作码 9–12，包含完整身份操作、认证绑定与固定预期根的安装。C/C++ SDK 与原有操作码保持兼容。
- FS 0.4 新增冷路径显示查询。NES 0.1.36/r42 启动时查询实际 SD 目录、缓存给菜单显示，替换写死的哈希。未增加逐帧文件扫描、验签或路径查询。
- 后端迁移 0007、作者/成员/应用绑定、开发者后台实际表单、独立签名域认证证明、精确详情查询均已实现。Store 0.8.15/r23 已构建并包含作者描述。

## 设备与数据

只操作 DOIT GameKit-1、ESP32-S31，设备序列号 `1c:29:04:d0:af:6a`，USB 标识 `5CFF040750`，串口 2000000。未操作另一块 GameKit、Korvo 或 PAI Touch。开始前确认同板 NES 任务的设备验收和收尾记录完成，重新备份了其最新版本及九个游戏。

固件仅写应用分区 `0x10000`，未写分区表、NVS 或文件系统镜像。最终镜像：

- ELF SHA-256：`99f485c1a0edc7b8f1c10f47de7016db63c0e38e8b771810af5010ada6e3cc38`。
- 应用镜像大小：4,008,864 字节，分区还余约 24%。
- 可复核的镜像哈希和操作范围：`local/pxa-identity-implementation-20261021/active-firmware.json`。

独立测试应用先在旧固件创建 8192 字节确定性文件。升级版禁止重新创建；首次迁移及修复后的连续两次冷启动均通过 Guest FS 读回原文件，SHA-256 为 `ce073871c9f2691ea6eee4ef8c3affa09033e4a423813b65d7632c2ee1eb3141`。首次重启测试发现元数据刷新把私有目录缓存重置到旧路径；已修复，原始短目录文件始终完整。诊断候选曾生成一个只含空 `.pxa-storage` 的旧测试目录，它不包含用户文件，也不参与最终应用解析。

另安装没有作者描述的旧格式应用，确认它继续使用旧目录且不占用作者名。两款测试应用均通过 Host 正常清理私有测试文件并卸载。

正式 NES 身份仍为：

```text
17982acd08493944059713aee9a56135dd4a080ecaaf7aa77df11fdf3102d171:pxa-nes
```

实际短目录：

```text
/pxa-state/packages/lucinhu/pxa-nes/
/pxa-state/data/lucinhu/pxa-nes/
```

最终固件冷启动日志确认 `NES LIBRARY flash=9 sd=0` 与 `NES SD media=/PXA/media/lucinhu/pxa-nes/`。九个 ROM 与两份原始电池存档均逐文件校验；迁移前后 11 个文件完全相同。

| 原始存档 | 字节 | SHA-256 |
| --- | ---: | --- |
| nes-6f2a4afc.sav | 8192 | e7c2d5109fcfa16eb15e153f552d95f0ea5f6aa4d8b5b7bb660f9181c312a2f2 |
| nes-ac316abf.sav | 8192 | f95d36e0e7a863bf3b7fa1cf5b199ef18e0e4d17dc433678ef6d0283fe0ab1d2 |

本次只在正式身份下运行无电池存档的马里奥。没有启动新的重装机兵游戏来改写用户进度。最终停在重装机兵的游戏库选择项；测试输入经过 `INPUT CANCEL`，恢复 GPIO 上报。控制器测试使用调试注入，不代表机械按键按压测试。

## 帧率与内存

马里奥 ROM 校验、mapper 0、PAL 50 Hz 已核对，确认进入实际关卡并能移动。开启音频，控制器输入自动隐藏悬浮按键，持续 RIGHT 与 A 脉冲，采集六个完整五秒窗口；截图及存储校验在采样外执行。曾出现锁屏的初次进入检查未作为性能数据。

| 项目 | 结果 |
| --- | --- |
| 模拟速度 | 49.89–50.19 Hz |
| 模拟积压 | 六窗均为 0 帧 |
| LCD 完成帧率 | 45.151 FPS |
| 音频状态 | 六窗 sound=1 |
| 悬浮按键 | 六窗 keys_auto=1、keys_alpha=0 |
| 音频丢弃采样 | 0、0、0、384、0、512 |

旧版 0.1.35 同 ROM、相同运动脚本的参考值为模拟约 50.029 Hz、LCD 43.742 FPS。动态游戏场景不同，不能将约 3.2% 的差异归因于目录改造；本轮没有观察到帧率回退，也不声称完全无音频丢弃。LCD 指标是固件完成提交频率，不是光学测量的面板刷新率。没有把 PAL 50 Hz 写成模拟 60 FPS。

NES 仍用固定 4 MiB 线性内存与原来的双 Surface 缓冲，没有为身份系统增加帧缓冲。激活及最终内存快照分别记录在 `local/nes-ui-20261021/identity-nes-reboot.json` 和 `identity-final-library.json`，原始采样位于 `local/pxa-identity-implementation-20261021/mario-performance/results.json`。

## 软件验收

- 后端全部 54 项测试通过，使用临时数据库；包含迁移升级/回退、并发唯一约束、权限、CSRF、同名精确查询与证明策略。
- Host 原生 ASan/UBSan 回归通过；目录注册表覆盖 CRC、截断、同名隔离、不确定提交，迁移覆盖九种失败/恢复情形。
- 实际 POSIX 适配器及 Core 回归通过。恢复丢弃未提交 incoming；拒绝的布局不能提升为安装结果。
- 服务端签发到原生 P-256 验证通过，覆盖未知 issuer、过期、无可信时间、逐字节篡改/截断、大于 JS 安全整数的 revision、跨签名域重放拒绝。
- C/C++ Store 0.7 请求一致性、C++ FS 0.4、桌面媒体查询/隔离/只读、包作者清单签名测试通过。
- 新 NES 的语言、分辨率、系统文字、滚动、音频开关及 SRAM 退出前端回归通过；S31 与模拟器包均已构建。

## 交付与边界

- NES：`local/pxa-identity-implementation-20261021/nes-package/pxa-nes.pxa`，设备已安装 0.1.36/r42。
- Store：`local/pxa-identity-implementation-20261021/store-package/pxa-store.pxa`，已构建，未在本设备安装。
- 固件：`local/pxa-identity-implementation-20261021/host-source/build/firmware/doit-gamekit-1/pxa_esp_platform.bin`，已在上述 GameKit 验证。隔离构建保留原先测过的游戏/控制器后端，未混入共享工作区其他渲染任务的改动。
- 完整记录与备份：`local/pxa-identity-implementation-20261021/`。历史 r40 元数据候选未安装，正式升级沿用另一任务最新的 0.1.35 代码再加入冷路径查询，没有回退其功能。
- SD 当前没有就绪卷，未声称完成实体 SD 卡 rename 测试；目录迁移状态机已做故障测试，显示查询及无卡 Flash 使用已实机验证。
- 服务器代码与数据库迁移文件已交付，未部署服务或修改现有数据库。生产认证需要持久私钥及产品固定的 issuer 公钥；默认固件支持离线作者目录，不信任网络自带公钥。
- 当前目录注册表上限为 16 作者/32 应用，保留卸载绑定以保护可移除媒体；耗尽时安全返回资源限制。现有 Flash 后端采用可读别名与原事务安装机制；尚未切换到提案中另行描述的代际卷 `/PXA/app/.../{release_id}` 布局。
