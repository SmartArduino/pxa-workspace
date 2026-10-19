# OpenLara0.5.0：保留玩法与敌人数的真机绘制优化

本次沿用完整TR1主引擎，保持活动敌人数、可见距离、关卡数据、碰撞和游戏逻辑。默认仍为320×240，增加可选240×180、160×120内部3D绘制；输出、标题、原版背包和平台菜单均保持320×240。两种性能档保持4:3视野，画面会相应变粗，设置可重启保存。原游戏存档格式保持不变，分辨率使用设置记录原先保留的一个字节。

版本`0.5.0`、release_sequence`36`，应用源码提交`277bf83`。本次没有修改固件；真机仍为已修复音量保存的固件`21c9d28c7a484873e2b46c782a965a60eec4f746886802ab85983332f5fb9b39`。

## 实测

DOIT GameKit / ESP32-S31，串号`1c:29:04:d0:af:5e`，运行S31原生AOT。使用同一公开PC演示版City起点，角色保持`78336,-1024,12800`、朝向0；计时窗口内持续检查手柄中立和角色未移动。以下是Host presentation FPS的一秒窗口统计，并非桌面模拟器帧率。

| 内部3D分辨率 | 样本数 | 平均FPS | 中位数 | 范围 |
| --- | ---: | ---: | ---: | ---: |
| 320×240，默认 | 20 | 8.10 | 7.9 | 7.9–8.9 |
| 240×180 | 20 | 8.90 | 8.9 | 8.9–8.9 |
| 160×120 | 60 | 10.23 | 9.9 | 9.9–10.9 |

上一版0.4.3在同板同固件、同一City起点的60个有效样本平均为7.32FPS。因此保留原画面时约提升11%；160×120相对旧版约提升40%，其中包含降低内部绘制分辨率的画质取舍，不能全部归因于代码优化。不同批次采样及一秒计数存在量化误差。这块板上目前仍只有约8–10FPS，不宣称达到20–30FPS。

160×120档完成20次120ms右转短按，40个按下/松开边沿统计：Host手柄状态采样到Guest提交画面平均135.0ms、最大170.0ms。此范围不含机械开关消抖和LCD扫描；输入通过GameKit controller-0调试注入，不是机械按键到发光像素的测量。行走、转向、原版背包往返、存档恢复、设备实际重启后恢复存档与分辨率设置均通过。

设备当前安装0.5.0，并保存为160×120。可在Select→Settings→3D resolution按A依次切换320×240、240×180、160×120。其他已安装应用的版本、安装/启用/前台状态在本次最终测试前后保持一致。完整结果见[summary.json](openlara-render-perf-20261019/summary.json)、[native320.json](openlara-render-perf-20261019/native320.json)、[native240.json](openlara-render-perf-20261019/native240.json)和[native160.json](openlara-render-perf-20261019/native160.json)。

## 代码改变

- 普通面片在计算光照、写入临时数组前，执行原扫描线已有的整数背面和屏幕边界剔除。只提前移除了原本不会绘制的面；保留原来的可见距离和面片判定。
- 对需要齐次裁剪的面先测试原有近/远深度平面，完全隐藏时省去X/Y计算；跨近面的可见部分仍完整裁剪。矩阵运算顺序保持一致。
- 动态光源列表为空时，跳过无用法线归一化和逆矩阵计算。
- 引擎直接写入Host已取得的空闲RGB565缓冲，提交后等待释放，省去整帧复制。
- 可选低分辨率只作用于绘制，在下一次逻辑更新前恢复原尺寸；使用倒序原地放大，不新增整帧缓冲。标题、背包和平台菜单保留原分辨率。

分项计时默认关闭。诊断包自身的逐批次计时调用会降低真机帧率，只在相同计时开关下比较。在两个诊断包中，提前剔除等改动使顶点阶段从约54ms降到44ms；基线诊断包平均6.25FPS，撤回无效实验后的诊断包平均6.8FPS。诊断数字不能混作正式包的FPS收益。

曾测试多个属性共享精确倒数乘法：183万组数值检查、360帧画面和音频都一致，但在本机使光栅阶段从约44ms增至约60ms，抵消了其他收益；该实现已从生产代码撤回。相关诊断证据保留为[profile-rejected-reciprocal.json](openlara-render-perf-20261019/profile-rejected-reciprocal.json)。

## 玩法和画面验证

[gameplay-source.json](openlara-render-perf-20261019/gameplay-source.json)核对敌人、角色、碰撞、关卡流程、存档和PHD解析源码与原提交逐字节相同。更关键的是，[resolution-gameplay.json](openlara-render-perf-20261019/resolution-gameplay.json)在相同操作序列后比较320×240、240×180、160×120的完整游戏存档，结果逐字节一致；涵盖转向、行走、跳跃、拔枪、动作及背包往返。两个性能档的设置跨实例恢复与存档读取通过，旧版存档兼容由升级回归另行验证，标题、背包、平台操作说明的画面保持一致。

原分辨率的[360帧比较](openlara-render-perf-20261019/full-resolution-pixels.json)与0.4.3画面、PCM音频完全一致。原版OpenGL对照的五个固定机位仍通过，并与上次修复深度遮挡后的软件输出完全一致，见[reference-pixels.json](openlara-render-perf-20261019/reference-pixels.json)。完整引擎回归、20次关卡切换内存稳定性、控制、输入慢帧与暂停、时钟协商、目录、存档升级均通过；设置页新增两行后，原有五项与标题仍逐像素比较，新增分辨率及B键返回由功能测试和SDL键盘实测覆盖。

独立ASAN/UBSAN测试覆盖深度、透明纹素、只读深度、近面裁剪、提前背面/屏幕外剔除；缩放测试用独立最近邻公式检查两个比例的全部输出像素与前后边界。实际SDL键盘路径通过移动、存档、背包、交换A/B、三个分辨率循环及160×120设置重启持久化，见[simulator.json](openlara-render-perf-20261019/simulator.json)。

本轮仍只实测公开City演示关，完整商业原版DATA未提供；不把这些结果当作15关逐关验收。原版数据导入入口与之前的未验证范围保持不变。

## 构建与复现

[build-source-binding.json](openlara-render-perf-20261019/build-source-binding.json)记录72个实际编译输入与6个构建产物的SHA-256。正式WASM与S31包已确认没有剖析输出代码；S31包通过`pxa publish --check-only`。源码、签名安装包及验证证据保存在`local/releases/openlara/0.5.0/`，源码归档不含原版游戏资源。

```sh
./tools/app.sh build openlara --board doit-gamekit --target esp32s31 --aot-only --output local/openlara-perf/rebuild
python3 local/pxa-apps/openlara/tools/classic-device-latency.py \
  --port /dev/ttyACM0 --serial 1c:29:04:d0:af:5e --version 0.5.0 \
  --package local/openlara-perf/rebuild/pxa-openlara.pxa \
  --output local/openlara-perf/recheck --resolution 160 --samples 60 --reboot-check
```

复测前先解锁屏幕并松开手柄。脚本若检测实体输入接管、角色移动、其他应用前台或遥测中断，会终止并保留失败状态；本轮曾遇到手柄接管及系统锁屏，相关失败批次未计入上述结果。最终有效批次使用带`-unlocked`的目录保存，两个性能档顺序测量以避免中间空闲锁屏。
