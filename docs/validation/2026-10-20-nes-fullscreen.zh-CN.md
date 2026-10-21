# PXA NES：全屏、十字悬浮按键与 GameKit 手柄验证

最终版本为 **0.1.16，release_sequence 17**。PAI Touch 已安装正式包，完整 256×240 游戏图像铺满 296×240 逻辑屏幕；十字方向键、右侧斜排 A/B 和中间功能键覆盖在画面上。手柄操作自动隐藏悬浮按键，触屏或全部手柄断开后恢复。

PAI Touch 使用用户指定的马里奥和重装机兵 ROM，模拟速度分别维持 PAL 50Hz 和 NTSC 60Hz。GameKit 完成独立的手柄功能测试；其 flash 空间不足，因此没有在 GameKit 上验收完整 AOT 模拟器的运行帧率。

## 操作变化

- 左侧十字方向键支持一个指针按斜向，中心有死区；可用另一指针同时按 A/B。按钮都避开底部 24 像素系统手势区，手势区也显示游戏图像。
- 按钮底色不透明度可切换 38% → 25% → 63% → 隐藏。文字、边线和按住时的反馈更清楚；透明混合从原始游戏像素计算，避免画面反复变暗。
- 收到手柄实际按键才自动隐藏，空闲连接不会隐藏；松开后保持隐藏。第一次触屏用于恢复原透明度，避免同时触发游戏动作；手动设定的隐藏状态仍保留。
- 手柄 X 暂停/继续。暂停菜单用上下选择、A 确认，B 或 START 继续；ROM 库用左右切换 PACK/FLASH/SD，上下选择，A 或 START 加载。菜单能完全用手柄操作。
- GameKit 的 START+SELECT 被系统保留给返回桌面，使用 X 暂停。桌面键盘 A 对应手柄 X；Z/X 对应 NES A/B。

全屏采用最近邻缩放，PAI Touch 的横向比例会相应拉伸，物理面板圆角仍遮住四角。按键颜色由 RGB565 查找表预先混合，新增静态内存约 104 KiB；线性内存保持固定 4 MiB。内核和调色板沿用上一版。

![PAI Touch 十字按键与双指跳跃](nes-fullscreen-20261020/pai-mario-jump.png)

## PAI Touch 实机帧率与按键开销

目标为 ESP32-S3 / PAI Touch，序列号 `a4:cb:8f:d5:d4:f0`，固件保持 Core 2.0 / PXA 0.3 / Clock 0.2 / FS 0.2。声音开启，默认 DISPLAY 1/4，保留自动追赶和显示跳帧。四个工况各重新启动游戏并完成相同的短操作流程，预热 8 秒，采集两个完整的约 5 秒窗口。窗口内只订阅实时日志和保持唤醒；截图、存储及内存查询位于窗口外。

| 游戏 | 按键 | 平均模拟 FPS | 两个窗口 FPS | Surface 提交 FPS | 每张绘制 ms | 最大积压帧 | 音频丢采样 |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: |
| 超级马里奥 PAL | 38% 悬浮按键 | **49.9775** | 49.62 / 50.33 | 4.765 | 19.843 | 1 | 0 |
| 超级马里奥 PAL | 隐藏按键 | **49.8842** | 49.71 / 50.05 | 4.975 | 18.025 | 1 | 0 |
| 重装机兵中文版 | 38% 悬浮按键 | **59.9372** | 59.97 / 59.89 | 5.985 | 19.764 | 0 | 0 |
| 重装机兵中文版 | 隐藏按键 | **59.9397** | 59.97 / 59.90 | 6.355 | 18.018 | 1 | 0 |

模拟 FPS 按实际完成帧数除以单调墙钟时间计算。马里奥 CRC `9a2db086` 被内核数据库识别为 PAL，正常满速为 50fps；重装机兵 CRC `6f2a4afc` 是 Mapper 74 / NTSC。所有窗口均通过目标帧率与积压检查，Guest 音频丢采样均为 0。Surface 成功提交数代表应用交给宿主的画面，并非光学测量的 LCD 刷新率。

`paint_avg_us` 包括缩放、按键查表更新和合成，不含 Surface 提交调用。以下为显示与隐藏按键的独立窗口均值之差，包含正常测量波动：

| 游戏 | 每张增加绘制时间 | 绘制时间增加 | Surface FPS 差值 |
| --- | ---: | ---: | ---: |
| 超级马里奥 PAL | 1.819 ms | 10.1% | -0.210 |
| 重装机兵中文版 | 1.746 ms | 9.7% | -0.370 |

半透明按键有绘制开销；在这些工况中模拟速度仍达到各自制式目标。全屏像素数从旧版 179×168 增至 296×240，显示提交率也有所下降。旧版小画面的 Surface 提交约为马里奥 5.79–6.17fps、重装机兵 7.74–7.77fps，详见[前一轮实测](2026-10-20-nes-games.zh-CN.md)。这只是初始场景的短测，未覆盖通关、所有地图或战斗。

[汇总](nes-fullscreen-20261020/device-summary.json)、[实时采样日志](nes-fullscreen-20261020/device-tests.log)、[开销计算](nes-fullscreen-20261020/overlay-cost.json)。此前 0.1.15 十字按键版本还做了每工况四个窗口的对照，见[原型汇总](nes-fullscreen-20261020/prototype-015-summary.json)；上表仅使用最终 0.1.16 数据。

## 颜色验证

继续使用本机 FCEUX 2.6.6 / new PPU 的未滤镜参考帧和相同的 Composite Direct FBX 64 色调色板，配置与来源见[上一轮颜色验证](2026-10-20-nes-games.zh-CN.md)。全屏比较区域为 `(0,0,296,240)`，按实际最近邻采样坐标比较编码后的 RGB565；排除顶部 20 行宿主 FPS 浮层。显示按键时排除九个控件矩形，隐藏时比较整个游戏区域。

| 实机标题 | 按键 | 比较像素 | RGB565 匹配率 | 调色板外像素 |
| --- | --- | ---: | ---: | ---: |
| 超级马里奥 PAL | 显示 | 56,876 | **100.0000%** | 0 |
| 超级马里奥 PAL | 隐藏 | 65,120 | **100.0000%** | 0 |
| 重装机兵中文版 | 显示 | 56,876 | **100.0000%** | 0 |
| 重装机兵中文版 | 隐藏 | 65,120 | **100.0000%** | 0 |

桌面截图另排除半径 58 的面板圆角和 `(120,231,56,4)` 宿主手势提示条，匹配率为马里奥至少 99.94%、重装机兵 100%，调色板外像素为 0。马里奥参考帧与采集帧的闪烁金币时序存在少量差异。混合区域的 RGB565 逐通道舍入和按下反馈由独立常量单元检查验证；游戏本身与半透明按钮颜色分别按各自规则验证。这不是 LCD 光学色温校准。

[模拟器颜色结果](nes-fullscreen-20261020/sim-colors.log)、[实机颜色结果](nes-fullscreen-20261020/device-colors.log)、[控件单元检查及 ASan/UBSan](nes-fullscreen-20261020/controls-test.log)。覆盖 296×240、200×160 和 320×320 控件边界、斜向、中心死区、底部手势区以及自动隐藏状态。

## GameKit 手柄链路

实机为 ESP32-S31 / DOIT GameKit，序列号 `1c:29:04:d0:af:5e`，既有 PXA 0.4 / Core 2.0 固件，串口使用 2,000,000 baud。实际查询得到 `controller_mask=4095`。输入通过 PXADB 注入板卡状态，由正常 50ms 板卡上报路径发送为 PXA controller-state 事件，再由 NES 读取。

- “右+A”映射到 NES 位 `0x81`，在诊断 ROM 暂停后的 8192 字节 SRAM 中读回该字节；文件 CRC32 为 `8d8648f1`，与原子替换日志一致。
- 诊断精灵包围盒从 `[158, 101, 167, 108]` 移动到 `[165, 101, 174, 108]`；按 A 也改变了诊断背景颜色。
- 自动隐藏、松开后保持隐藏、注入触控恢复、断开事件恢复均有截图和状态日志。
- X 持续按住不会反复暂停/继续；全手柄选择暂停菜单、返回 ROM 库、左右切换来源和重新加载游戏通过。

GameKit 原 flash 仅余 **380,928 字节**，完整版放不下。本轮使用独立身份 `pxa-nes-padtest` 的解包有效载荷 139,266 字节（约 136 KiB）的测试包：保留与生产一致的 `main.c`、`controls.c/h`，只保留 NROM，移除 ROM 数据库，采用 `-Oz` 和 WASM 解释执行。它用来验证手柄功能，**其解释器帧率不作为完整版速度结论**。完整内核的 ESP32-S31 AOT 已在本地多架构测试包中构建，尚未在 GameKit 做运行速度验收。没有更改原应用或刷写固件。

这轮没有人工按压机械按键，也没有测试外接蓝牙/USB 手柄连接。应用消费宿主的标准手柄事件；PAI Touch 旧固件的外接手柄驱动仍属于设备端能力。

[GameKit 测试结果与内存](nes-fullscreen-20261020/gamekit/results.json)、[输入及保存日志](nes-fullscreen-20261020/gamekit/controller.log)、[前端源码一致性 SHA256](nes-fullscreen-20261020/frontend-identity.json)。

![GameKit 手柄输入后隐藏悬浮按钮](nes-fullscreen-20261020/gamekit/right-a-hidden.png)

![GameKit 长按 X 保持暂停](nes-fullscreen-20261020/gamekit/x-held-paused.png)

桌面也验证了 SDL 控制器事件、右+A、释放后保持隐藏、触摸恢复、持续按住暂停组合、X 暂停及纯手柄菜单，见[控制器结果](nes-fullscreen-20261020/sim/controller-results.json)。

## 内存、存档与设备收尾

PAI Touch 运行重装机兵并启用声音后的系统快照：可用 PSRAM **2,200,644 字节**、可用 SRAM **34,415 字节**；宿主预算拒绝 `0`、分配失败 `0`。历史最低内存不限定于本轮，完整当前值和历史边界见[运行内存](nes-fullscreen-20261020/device/metal-max-cn/on/performance/memory.json)。声音未做人工听感或声学延迟测试。

重装机兵暂停后验证 SRAM 文件长度和 CRC 与成功保存日志一致，见[保存验证](nes-fullscreen-20261020/persistence-final.json)。原始 ROM 的校验值保持不变，见[原文件完整性](nes-fullscreen-20261020/rom-integrity.json)。

PAI Touch 的临时 `pxa-nes-fullscreen` 已停止并卸载，正式 `pxa-nes` 升级为 **0.1.16 / release_sequence 17**，保留原身份与私有存档目录。最后重新加载正式包的诊断 ROM，以屏幕上的 MENU 按钮暂停，声音关闭；原浏览器保留。[最终 PAI Touch 状态](nes-fullscreen-20261020/final-device.json)。

GameKit 临时 `pxa-nes-padtest` 已停止并卸载，安装前后的五个原应用、版本和非活动状态完全一致，见上述 GameKit 结果。双方均未格式化存储或更新固件。自行启动的桌面模拟器与调试服务已停止。桌面模拟器在收到清理用 SIGTERM 后返回 134，日志没有提供退出原因；自然关闭流程没有计为通过。设备端 Package stop、卸载和重新运行验证通过。[桌面收尾记录](nes-fullscreen-20261020/simulator-cleanup.json)。

正式包：`local/app-output/pai-touch/pxa-nes.pxa`，随包仅有原创 CC0 诊断 ROM。用户 ROM 仅在本地忽略目录的临时签名测试包中。

[源码 SHA256](nes-fullscreen-20261020/source-sha256.json)、[签名包与 AOT SHA256](nes-fullscreen-20261020/artifact-sha256.json)、[正式构建](nes-fullscreen-20261020/build-production.log)、[完整多架构测试构建](nes-fullscreen-20261020/build-test.log)、[实机所用 S3 包构建](nes-fullscreen-20261020/build-test-pai.log)、[正式安装日志](nes-fullscreen-20261020/install-production.log)。原有内核兼容性边界未变，组合指令测试 `official_only.nes` 尚未通过，详见[内核报告](2026-10-19-nes-pxa.zh-CN.md)。
