# 固定版本 ESP-IDF Host SDK 接入

这是固件集成包。开发 PXA App 使用 Guest DevKit；让现有固件运行 App 使用本包。
包内含固定的 libpxa、PXA System、WAMR、PageManager、ESP Host、系统桥接、
LVGL 输入同步和 PXADB。它不包含板级驱动、产品主程序、旧应用或开发私钥。

## 固件依赖与产品边界

产品提交 `pxa-host-sdk.lock.json`、配套归档（或归档的固定下载地址）和产品适配。
锁包含 PXA 版本、Host 集成修订、四个源码提交、归档及文件清单 SHA-256。
离线归档安装后位于忽略的 `deps/`。配置阶段校验所有文件；修改、增加或删除包内
源码会失败。修复通用代码应在上游完成、重新发布修订包，再更新产品锁。

Host 集成包使用 `<PXA发行号>+idf.<修订>`；不同 ESP 适配修订可使用同一固定核心。
这不会改变 Core/Service/AOT 的运行兼容规则，不要求 App SDK 与固件发行号相等。

包依赖产品已有 ESP-IDF、LVGL 9、LittleFS、LZ4、mbedTLS 及 ESP Audio Codec。
按实际产品锁定 ESP-IDF 和组件依赖；包内不会附带第二份 LVGL 或初始化硬件。

## 快速接入

```sh
python3 scripts/pxa_host_sdk.py install --lock pxa-host-sdk.lock.json
```

在项目根 CMake 的 `project.cmake` 之前包括安装目录的
`cmake/pxa_host_sdk.cmake`，在它之后、`project()` 之前调用
`pxa_host_sdk_configure()`。安装目录从锁读取，不维护第二个版本常量。

```cmake
file(READ "${CMAKE_CURRENT_LIST_DIR}/pxa-host-sdk.lock.json" pxa_lock)
string(JSON pxa_sdk_dir GET "${pxa_lock}" install_dir)
set(PXA_HOST_SDK_UI_BACKEND app_pages CACHE STRING "" FORCE)
set(PXA_HOST_SDK_BUILTIN_AUDIO OFF CACHE BOOL "" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/${pxa_sdk_dir}/cmake/pxa_host_sdk.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
pxa_host_sdk_configure()
project(my_product)
```

已有 `app_pages` 产品采用 `app_pages` 后端；其他产品采用 `standalone`，或后续扩展
公开的 UI provider。SDK 不自动创建 PXA System 桌面。保留产品自身的组合入口，
只创建一次 System、LVGL display、触摸路由及音频输出。
依赖 `pxa_lvgl_input` 以启用输入读取同步；不创建额外任务、缓存或互斥锁。

产品适配应使用 `pxa/pxa_host.h`、`pxa/pxa_esp_surface.h` 及
`pxsys/esp_pxa_bridge.h` 等公开接口；不得包含 Host 的 `src/` 私有头。
窗口安全区、DPI、圆角与旋转来自产品显示配置。系统弹窗须正确通知覆盖状态，
前后台与关机路径须通知 Host，防止隐藏游戏持续运行或丢失保存机会。

## 音频所有权

已有 GMF/通话混音器的产品设 `PXA_HOST_SDK_BUILTIN_AUDIO=OFF`，通过公开 PCM、
声音对象和音乐 sink 注册实际支持的能力。不要把独立输出的 codec 分配钩子链接
进原生媒体系统。未实现的后端操作应返回不支持，并准确发布能力。
采用独立输出的产品可启用内置 `PxaAudioOutput`；它的解码内存受 PXA 预算约束。

## 存储与应用

Host 使用产品已经挂载的 LittleFS，分区标签、挂载点和状态根须保持一致。
禁止为了接入而格式化已有分区。保存发布者身份、App ID、权限、安装目录与私有
数据；AOT engine ABI 不匹配的旧包需要重编译，不能绕过检查。
应用打包与固件构建独立。出厂应用如有需要，可另行置备已签名包。

## 制作、升级与验证

```sh
python3 tools/host-sdk/pxa_host_sdk.py package \
  --workspace /path/to/committed/pxa-workspace \
  --system /path/to/clean/pinned/pxa-system \
  --revision 1 --output /path/to/release
python3 tools/host-sdk/pxa_host_sdk.py verify \
  --root /path/to/installed/sdk --lock /path/to/product/pxa-host-sdk.lock.json
```

打包器拒绝脏源码及未匹配的子模块。归档可重复生成，不包含绝对工作路径和构建
时间，不覆盖已有归档。版本升级产生新的安装目录，原目录保留用于回退。
更新锁及归档后运行同一安装命令；不自动跟随 latest/main。

验收至少覆盖：固定来源与篡改拒绝、产品桥接原生测试、IDF 构建、安装/升级/卸载、
C/C++ 应用、显示输入/系统弹窗、前后台/重复启动、原生媒体共存、稳定及本轮峰值
内存。真机先核对实际分区并备份；应用分区更新不应包含 bootloader、分区表或数据。

启用 PXADB 时必须关闭 `CONFIG_LIBC_NEWLIB_NANO_FORMAT`（旧 IDF 名称为
`CONFIG_NEWLIB_NANO_FORMAT`）。PXA 使用 64 位时间戳及计数；newlib nano
不支持 `%llu`，除了统计错误还会使后续 `%s` 错读参数，在截图时触发崩溃。
组件配置会拒绝这种组合。保留完整格式支持，不以这项代码体积节省换取功能风险。

`paixueji_nertc_demo_new` 是保留旧桌面与共享 GMF 音频的消费示例。
具体硬件、业务调用与验证限制由产品接入报告说明。

## 0.3.0 集成修订

Host SDK 基于已提交的 PXA 0.3.0 / Core 2。产品可选择 `app_pages`
后端，并关闭 `PXA_HOST_SDK_BUILTIN_AUDIO`，复用已有原生音频管线。
独立固件仍默认使用自带输出模块。离线包通过已校验的清单应用 WAMR
补丁，补丁输出只写入构建目录，不需要 Git 元数据，也不修改 SDK。

打包源必须是干净、已提交的检出目录；主工作区有改动时使用独立 worktree，
不能把未提交的板级改动无意带入通用 SDK。
