# DOIT GameKit 锁屏解锁崩溃修复

## 故障与定位

用户报告桌面和其他应用也会在锁屏解锁时崩溃。故障位于共用的
GameKit 手柄导航模块，与某个游戏的渲染或音频无关。

从目标设备（`1c:29:04:d0:af:5e`）读取当前固件，确认应用描述中的 ELF
SHA-256 为 `c600dde245980b89b422b6b62f1efff9f2b50a3ee4a8066c340b4f876817f0a1`，
与用户日志一致。没有使用哈希不匹配的旧 ELF 解释地址。

实际镜像中 `0x401e0e58` 是 `lw a0,4(a0)`，位于沿父对象链向上遍历的
`lv_obj_is_in_widget_tree()`；`lv_obj_is_valid()` 在 LVGL 9.6 中是它的兼容名称。
日志中的 `a0=0x69`、`MTVAL=0x6d` 与这条指令吻合。返回地址 `0x4001a984`
对应内联在 `PollInput()` 中的 `KeyNavigation::Cancel()`：它检查保存的
`pressed_object_`，而该对象已经在 Enter 解锁回调中删除。

`lv_obj_is_valid()` 不能用于检查已经释放的地址。锁屏对象释放后的内存是否
被复用决定了是否立即崩溃，也解释了桌面和不同应用都能触发。

## 修复

按下目标使用 `lv_obj_null_on_delete()` 监听删除；删除事件立即清除引用。
释放、取消或更换目标时移除旧监听，避免旧控件以后删除时影响新的按下目标。
输入框在 Enter 回调中删除时也不会继续激活编辑器。

没有新增任务、队列或成员缓冲区。持续按住同一对象时不会重复注册监听；
监听数量保持一个，释放后恢复原事件数量。

## 本地回归

`test_navigation.sh` 现在同时为真实 LVGL 库启用 ASan/UBSan。只对测试程序
启用检查、链接未检查的 LVGL 归档会漏掉本次错误。

修复前，新回归稳定报 `heap-use-after-free`：

```text
lv_obj_is_in_widget_tree          lv_obj.c:1305
KeyNavigation::Cancel            doit_gamekit_navigation.h:59
main                             navigation_test.cc:188
freed by lv_obj_delete in the Enter event callback
```

修复后通过：

- 100 次 Enter 回调内删除锁屏对象，交替返回桌面焦点模式与游戏控制器模式。
- 持续按住不增加删除监听，释放后移除监听。
- 删除旧按下目标不影响当前目标的取消事件。
- Enter 回调内删除输入框，不继续激活已经释放的输入框。
- 原有真实 LVGL 导航、内容刷新、本地输入法与 PXA Provider 输入法测试。
- GameKit 输入、合成器与旋转测试。

复现命令：

```sh
CXX=clang++ CC=clang bash firmware/boards/doit-gamekit/tests/test_navigation.sh
bash firmware/boards/doit-gamekit/tests/test_host.sh
```

## 设备证据

本次调试产物位于 `build/tests/doit-gamekit/lock-unlock-20261020/`。
刷写前已读取 `0x000000..0x410000`；其中原应用镜像通过 esptool 的校验和与
SHA-256 验证。分区为 factory `0x10000/0x500000`、pxa_data
`0x510000/0xaf0000`。备份包含 NVS，仅保留在本地忽略目录中。

已向目标设备的 `0x10000` 写入修复应用，esptool 完整校验成功：

- 应用 SHA-256：`52f3031f6fdfef5571a259757aaa8884c345157b23e7966c0a8e6829f5a9d9f9`。
- ELF SHA-256：`9554d0eaf6d8d8ec55888c4971b225c3a92109bb891606fc30c9483cc5aa89d2`。
- 设备重新连接后报告 Core 2.1，原有三个应用仍已安装。
- 桌面等待 130 秒：截图确认进入锁屏，上滑解锁后返回原桌面。
- NES 0.1.22 等待 130 秒：截图确认进入锁屏，日志出现后台/前台生命周期切换，
  上滑解锁后恢复 NES 资源菜单，应用仍为 active。
- 两轮设备日志未观察到 panic 或重启。

设备自动回归使用 PXADB 指针输入；实体 A 键解锁待用户复测。
按键删除路径已有上面的真实 LVGL ASan/UBSan 回归，不能将指针设备测试
描述为已经验证了实体 GPIO 按键。

固件以主仓库 `10f39c4`、SDK `5a6e179` 和本次导航修复构建。
构建时工作区已有 Surface 缓冲区生命周期修改，其准确文件哈希保存在
`source-manifest.json`；本次提交未收录其他任务的 Surface 或 Korvo 优化代码。
可公开保存的日志、截图和镜像元数据位于同名证据目录；完整镜像与备份保留在
本地 `build/` 中。
