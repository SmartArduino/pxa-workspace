# ESP Adapter Source Layout / ESP 适配源码结构

This directory is product integration, not portable PXA protocol code. Public
firmware APIs remain in `../include/pxa`; portable code remains in
`../../../../deps/pxa-system/libpxa`.

本目录只包含产品侧集成代码，不包含可移植的 PXA 协议实现。固件公共 API 位于
`../include/pxa`，可移植实现位于 `../../../../deps/pxa-system/libpxa`。

| Directory | Responsibility | 职责 |
| --- | --- | --- |
| `runtime/` | Boot, activation lifetime, owner-thread dispatch and public facade | 启动、激活生命周期、宿主线程调度和公共门面 |
| `services/` | ESP Audio, Net, Surface and service registration | ESP 音频、网络、Surface 和服务注册 |
| `package/` | Trust, repository, install policy, permissions and catalog projection | 信任、仓库、安装策略、权限和目录投影 |
| `ui/` | Neutral UI shell, assets and `app_pages` binding | 中立 UI 外壳、资源和 `app_pages` 绑定 |

Dependencies flow from `ui` and `runtime` into the package/service modules,
then into `libpxa`. Product UI types must not enter the package store or
portable core.

依赖从 `ui`、`runtime` 流向 package/service 模块，最终进入 `libpxa`。产品 UI
类型不得进入 Package Store 或可移植核心。
