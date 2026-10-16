# 产品固件的直出游戏与系统覆盖层

游戏 Surface 可以直接送往屏幕；状态栏、通知面板、系统对话框、音量提示和
Toast 出现时，板级显示适配必须切回合成。仅停用直出或重画 LVGL 背景不能正确
显示半透明系统 UI。

产品在 LVGL 所有者线程调用 `pxa_esp_system_overlay_set_reference_objects`，
发布实际可见的可信系统 UI 根对象；首次发布会自动启用透明层提供者，无需调用
SDK 私有函数。最多支持 `PXA_ESP_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS`（24）
个根对象。隐藏对象会被排除；销毁对象前须更新列表。不要传入桌面、应用或游戏根。
旧 app_pages 产品未发布列表时仍使用原有不透明回退，保持集成兼容。

在布局完成、显示刷新开始时更新缓存，合成顺序为：LVGL 背景、游戏 Surface、
Guest HUD、系统 RGB565+A8 层；`suppress_guest_alpha` 为真时跳过 Guest HUD。
系统层使用逻辑屏幕坐标，不能随低分辨率游戏 Surface 放大。必须同时检查
`ui_alpha_plane` 和 `system_alpha_plane`，避免直出覆盖系统图像。
缓存失败时 SDK 提供不透明 LVGL 回退；板端不能把 `visible` 为真、像素为空的
系统层解释为“没有系统 UI”。系统层关闭后遵守 SDK 的完整帧恢复屏障。

缓存更新只由 UI 布局/样式/动画变化触发。游戏为调度合成而执行的微小无效化
应单独标记，复用已有系统层；否则每个游戏帧都会产生 ARGB 截图与 PSRAM 分配。
空列表的重复发布不会再次唤醒 presenter，避免“刷新→发布→唤醒→刷新”的循环。
系统根按 LVGL 的祖先分支和屏幕/Top/System 层次排序，避免不同父节点下的状态栏
错误覆盖对话框。

没有新增游戏或屏幕帧缓冲。系统覆盖出现时仍需要既有 SDK 的按包围盒分配的
RGB565+A8 缓存和逐对象 ARGB 截图临时空间，隐藏后释放；因此不能声称显示系统
覆盖与纯游戏直出具有相同内存占用。注册表从 8 增至 24 项，在 32 位目标上增加
64 字节静态指针空间，排序栈相应增加 64 字节。

原生回归：`SANITIZE=1 bash tools/test-system-overlay-cache.sh`，以及
`PXA_TEST_SANITIZERS=1 bash tools/test-game-system-gestures.sh esp-mosaico`。
覆盖自动启用、空刷新无唤醒、全屏启动隐藏旧状态栏、跨父节点顺序、12 个并存
根对象、64 字节对齐、状态栏临时显示、手势、Home/恢复与缓存释放。
产品板级像素测试还必须检查旋转、字节序、双核分块及游戏缩放。
