# OpenLara0.5.2：保持320×240的真机优化

本轮固定320×240，保留原版玩法、活动敌人数、视距、透明纹素、深度测试/写入和近面裁剪。应用源码提交`21e4ec4`。断线前设备已安装0.5.2、release_sequence38，并保存为320×240。固件与音量修复保持上一轮版本，SHA-256为`21c9d28c7a484873e2b46c782a965a60eec4f746886802ab85983332f5fb9b39`。

## 真机结果

DOIT GameKit / ESP32-S31，串号`1c:29:04:d0:af:5e`，原生RISC-V32 ILP32F AOT。以下均为同一公开PC City演示关起点`78336,-1024,12800`、朝向0，按秒检查手柄中立与位置不变，使用Host presentation FPS。正式包剖析关闭，分辨率均为320×240。

| 版本 | 改动 | 秒窗口数 | 平均FPS | 中位数 | 范围 |
| --- | --- | ---: | ---: | ---: | ---: |
| 0.5.0 | 本轮重新测量的基线 | 30 | 7.97 | 7.9 | 7.9–8.9 |
| 0.5.1 | 仅主单元O1→O2 | 60 | 8.42 | 8.9 | 7.9–8.9 |
| 0.5.2 | O2及O3扫描线批次/内联 | 60 | 8.82 | 8.9 | 7.9–9.9 |

相对本轮基线平均提高10.7%。一秒窗口存在计数量化误差，约9FPS仍未达到流畅水平；不将桌面模拟器或固定时间Node运行当作真机FPS。完整数据见[summary.json](openlara-320-perf-20261019/summary.json)和[native320-timing.json](openlara-320-perf-20261019/native320-timing.json)。

## 计算主要花在哪里

直接使用正式包原有帧计数器，在手柄中立计时窗口内累计，未添加逐面片时钟调用。[frame-stages.json](openlara-320-perf-20261019/frame-stages.json)记录原始累计量和算法；`render_us`本来包含逻辑，因此绘制用`render_us-logic_us`计算。

| 每帧耗时 | 0.5.0 | 0.5.2 |
| --- | ---: | ---: |
| 绘制与清屏 | 92.67ms | 82.64ms |
| 游戏逻辑 | 12.69ms | 11.61ms |
| 画面转换/复制 | 0.014ms | 0.019ms |
| 帧内区间之外 | 18.56ms | 18.01ms |
| 完整帧周期 | 123.92ms | 112.28ms |

绘制占最大部分。先前相同软件后端的诊断包把这一部分拆为约44ms顶点变换/裁剪/光照、42ms光栅化、3ms清屏；见[上一轮剖析](2026-10-19-openlara-render-perf.zh-CN.md)。这些带逐DIP计时的旧诊断数字仅用于定位，包含计时扰动，不能当作新版正式包的精确分项或FPS。几何部分包含矩阵乘法、投影除法、面片判定与必要的齐次裁剪；光栅部分包含边缘设置、逐扫描线的Z/UV/光照插值、纹理和调色表访问、深度比较与颜色写入。它们都有相当大的固定面片/扫描线开销，所以降低像素数的收益有限。

“帧内区间之外”包含音频、Host服务、调度与等待，当前计数器没有把这些继续分开，不把整个余量归到单一模块。每帧逻辑耗时也随每帧涵盖的仿真步数变化，不将它的全部下降归因于编译器。

## 改动与验证

主引擎C++单元从O1改为O2。PXA四边形/三角形设置、边缘步进与像素循环放入同一个O3单元，使用共享固定点顶点类型与每批次状态快照，强制内联像素内核。编译后的WASM重定位表中只剩进入`drawPart`的5处调用，扫描线循环没有调用外部像素函数，见[inline-proof.json](openlara-320-perf-20261019/inline-proof.json)。保留通用整数扫描线作为参考；没有启用fast-math，未换成近似除法，未新增整帧缓冲。S31 AOT为2,719,760字节，略小于0.5.0的2,737,360字节。

与0.5.0的360帧RGB565、PCM和完整游戏存档逐字节一致，覆盖转向、行走、跳跃、拔枪、动作、观察和慢走，见[pixels-audio-checkpoint.json](openlara-320-perf-20261019/pixels-audio-checkpoint.json)。另与修复深度后的0.4.3比较360帧与PCM；旧适配层内部状态初始化不同，跨代存档测试检查可恢复与双向读取，完整字节相同比较使用0.5.0。九项回归通过，包含20次关卡切换内存稳定性，见[regression-summary.json](openlara-320-perf-20261019/regression-summary.json)。

4000组三角形/四边形将新批次绘制与通用扫描线的完整颜色及深度缓冲比较，覆盖绕序、非零裁剪框、透明索引、深度测试/只读写入。ASAN/UBSAN通过；排除了原有有符号固定点位移的shift检查，见[raster-sanitizers.log](openlara-320-perf-20261019/raster-sanitizers.log)。[gameplay-source.json](openlara-320-perf-20261019/gameplay-source.json)核对游戏、敌人、碰撞、流程、存档及解析源码不变，雾和可见距离常量也保持原值。

真机完成完整60秒中立计时、转向/行走、存档恢复和原版背包。随后20次短按测试中USB重新枚举并消失，未完成短按和物理重启复验，也未验证本版320×240设置的物理重启恢复。0.5.1曾完整通过，但不把它当作0.5.2的重启验收。实际SDL键盘手柄路径通过移动、存档、背包、交换A/B及设置重启，见[simulator.json](openlara-320-perf-20261019/simulator.json)。

[native320-timing.json](openlara-320-perf-20261019/native320-timing.json)仅声明已完整完成的60秒静止计时通过，来源进程的整轮控制验收仍是失败，原始`passed:false`及USB异常保留于[native320-interrupted-controls.json](openlara-320-perf-20261019/native320-interrupted-controls.json)。另一次上传与首次不完整计时也遇到USB重新枚举，不完整计时未纳入统计。内核日志确认USB桥断开，重连时设备uptime继续增长；最后USB节点完全消失。用户恢复连接后可补做短按和重启检查。

全流程只测公开City演示关，完整商业DATA未提供，不将此结果当作15关验收。

## 构建与交付

[build-source-binding.json](openlara-320-perf-20261019/build-source-binding.json)绑定72个实际编译输入与6个产物，正式WASM及AOT确认不含剖析输出。签名S31包通过`pxa publish --check-only`。固件没有重新构建；SDK工作区另有并行PNG改动，使用的Guest头文件单独记录于[guest-sdk-inputs.json](openlara-320-perf-20261019/guest-sdk-inputs.json)。源码、签名S31/WASM包与证据归档位于`local/releases/openlara/0.5.2/`，不含原版商业游戏资源。

```sh
./tools/app.sh build openlara --board doit-gamekit --target esp32s31 --aot-only --output local/openlara-perf/rebuild-320
python3 local/pxa-apps/openlara/tools/classic-device-latency.py \
  --port /dev/ttyACM0 --serial 1c:29:04:d0:af:5e --version 0.5.2 \
  --package local/openlara-perf/rebuild-320/pxa-openlara.pxa \
  --output local/openlara-perf/recheck-320 --resolution 320 --samples 60 --reboot-check
```

运行前解锁并松开手柄；脚本遇到实体输入接管、角色移动、其他应用前台或遥测停止会将整轮标记为失败。
