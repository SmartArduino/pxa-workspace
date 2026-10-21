# AOT 真机加载对照

正常固件始终采用分段读取。本目录仅用于在**同一份测试固件**内切换普通 AOT 的
分段／整文件路径；不加入正式构建，也不改变 Guest 代码、AOT 文件或帧循环。

## 准备

1. 备份实际设备的应用分区和配置，确认真实分区表、挂载目录及存储分区名。
   已有产品的布局可能与 standalone 板配置不同；测试配置应关闭挂载失败时格式化。
2. 用 `pxa_wamr_aot_reader.c` 的目标编译参数编译 `buffered_reference.c`，另加
   `deps/pxa-system/libpxa/adapters/wamr` 的 include 路径。Xtensa 使用 `-mlongcalls`。
3. 将该对象和 `-Wl,--wrap=pxa_wamr_read_aot` 加入**测试固件**链接参数。
   默认标记文件在 `/assets/pxa-state/inbox/aot-buffered-reference.flag`；其他挂载
   点通过编译宏 `PXA_AOT_REFERENCE_FLAG` 改写。设备端用 `open()` 检查标记，
   不依赖某些 VFS 未实现的 `access()`。
4. 只更新已确认的应用分区；保留分区表、救援镜像和数据。安装相同签名的测试包，
   事先处理媒体权限等首次运行对话框，并停止其他应用。

标记存在时，包装器要求引擎采用现有整文件加载路径；标记不存在时调用正式的
分段加载器。Wasm 不经过这个包装器，可作为系统状态的对照。

## 内存测量

```sh
python3 tools/aot-loader/compare.py \
  --port /dev/ttyACM1 --baud 2000000 \
  --expected-serial 28:84:85:63:b7:78 \
  --apps pxa-cpp-gestures pxa-loader-wasm pxa-novel-reader pxa-jump-jump-3d \
  --wasm-apps pxa-loader-wasm --pairs 2 --output local/aot-compare
```

每应用先预热，再执行两轮“整文件、分段、分段、整文件”，共 32 次测量。
每轮启动前开始本地堆最低值窗口，激活后等待 1.2 秒停止窗口，再取五次运行占用。
截图、HOME 切换和输入不进入这些窗口。`active_ms` 含串口与状态轮询，不是首帧
或加载器耗时。脚本验证分段日志、保存完整响应，并在退出时移除自己创建的标记。
脚本会拒绝预先存在的标记；断电或强制杀进程后需检查残留。

若需分开统计首次初始化，应在重启后另建输出目录，先单独记录首次启动；不要将
预热后的局部峰值和 `MEMORY` 的历史开机最低值混用。

## 操作与画面

```sh
python3 tools/verify-device-aot-loader.py --port /dev/ttyACM1 --baud 2000000 \
  --rounds 3 --output local/aot-device

PXADB_BAUD=2000000 python3 tools/measure-device-gameplay.py \
  --port /dev/ttyACM1 --expected-serial 28:84:85:63:b7:78 \
  --app pxa-jump-jump-3d --package <实际安装的签名包.pxa> \
  --size 296 240 --scenes idle charge --repeat 2 --seconds 3 --warmup 1 \
  --screenshot-format raw --output local/aot-game
```

这些设备参数仅是本轮 pai-touch 示例。修改为实际设备的身份、尺寸与包。
`verify-device-aot-loader.py --grant-media` 只适用于已知的 296×240 测试对话框。
拒绝测试使用另行构造并签名的 QA 包，通过 `--reject-apps` 指定，不能指定正常应用。

游戏截图需检查 `source`、帧编号及实际像素。启动后仍可能先捕获到 LVGL 旧画面；
本轮使用测量结束后的 `source=surface` 截图验证游戏。串口注入延迟不等于物理
触摸到光子的延迟。帧率取面板完成间隔，动态场景同时报告整窗口帧率。

## 收尾

移除标记、测试应用及数据，恢复原固件与配置。移除链接包装器和对象，恢复正常
SDKCONFIG 后重新构建，并检查 ELF 不含 `__wrap_pxa_wamr_read_aot`。某些 IDF
版本将参数写入 `build/.../toolchain/ldflags`；只清空 CMake 缓存变量并不足以移除。

日志、采样数据与测量报告统一写入 `local/artifacts/aot-loader/<YYYYMMDD-HHMMSS>/`。
