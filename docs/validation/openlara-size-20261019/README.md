# 代码体积实验与真实 Lara 存档验证

`profiles.json` 是已完成的 0.3.4 实验产物记录。原版 PC PHD 玩法、软件渲染和原始资源保留；Oz/TR1-PC 实验未成为发布默认，未经硬件性能验证。

`wasm-footprint.py` 分析本项目仅导入函数的 WASM，列出代码/数据段及最大的函数；它不是通用 WASM 验证器。`pc-size-experiment.patch` 在独立源码副本内应用，用原工具链构建。S31 包已进行本地签名/安装规则检查，未写入设备。

`lara-integration.json` 使用更新后的实际 Guest 测试：根据 PHD 的 Lara 索引定位记录，检查移动、保存、转向、读档和跨实例恢复。第一个实体是机关，不再将它当作玩家。`upgrade-proof.mjs` 使用应用中的 `classic-save-probe.mjs`，需从 workspace 根目录执行并传入基线 WASM、候选 WASM、含 game/ 的测试资源目录及输出 JSON。

公开演示测试资源建议准备到独立目录，避免覆盖已导入的完整资源：

```sh
python3 local/pxa-apps/openlara/tools/prepare-data.py --demo \
  --output local/openlara-full/pc-demo-test/game --upstream local/upstream-openlara
node local/pxa-apps/openlara/tools/classic-integration.mjs main.wasm \
  local/openlara-full/pc-demo-test integration.json
```

全关卡数据仍待用户提供；常规 AOT 安装仍待足够空间。

`0.3.5-*.json` 是应用提交 `e0ebd7e` 的最终结果，覆盖真实 Lara 读档、默认与交换后的手柄、360 帧一致性和实际 SDL 键盘/进程重启。SDL 截图保留交换后选择、取消和重启状态。`0.3.5-artifacts.json` 同时列出已安装模拟器 AOT，散列与构建产物一致。体积实验仍以 0.3.4 为基线，不混作 0.3.5 默认产物。

`interpreter-*` 为独立 ID 的真机原始 WASM 诊断。`interpreter-s31-result.json` 的 1.9 FPS 中位数仅代表解释器；初始化有一次超时提示，正式采样期间无超时，逐次验证活动应用。首次触点误操作的帧率全部排除，弹窗截图单独标记 invalid。`interpreter-device.py` 保留安全采样模式，执行前需准备独立 probe 源目录及签名包；应用工具提交 `359dfa5` 另修正常规采样的超时/前台检查。
