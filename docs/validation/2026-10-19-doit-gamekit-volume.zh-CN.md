# DOIT GameKit：立即重启时恢复音量

`Gamekit::SetVolume` 先前只更新 MAX98357A 软件增益，系统状态和 NVS 保存要等下一次 1 秒轮询。调音量后立即重启，最后一次变化可能尚未进入 NVS。

现在更新音量后同步调用 `PublishStatus()`，沿用集成层的状态观察者保存 NVS。该路径覆盖 SELECT+L/R、调试音量键以及系统音量控件。开机恢复仍读取原 `pxa_user/volume`，支持 0% 静音，不改变存储格式。相同值不会反复提交 NVS。音量应用日志用于核对实际驱动状态。

使用实际生产 `SetVolume`、`LoadUserPreferences` 和 `SaveLevels` 函数体做宿主回归，模拟调音量后不等待状态定时器便重启。旧代码在首次保存断言失败；新代码通过 35%、0%、100% 恢复与重复写入抑制测试。启动顺序测试也通过；其字体路径 mock 已适配现用 getter。

固件在独立目录使用干净 SDK `dc9365e2da4a4187991a22b40e1276f26a2cc448` 构建，排除其他任务未提交的 PNG 修改。最终镜像 SHA-256 为 `21c9d28c7a484873e2b46c782a965a60eec4f746886802ab85983332f5fb9b39`，来源与实际刷入记录见 [firmware.json](doit-gamekit-volume-20261019/firmware.json)。实际构建输出包含音量诊断字符串，编解码内存审计通过。

真机为 DOIT GameKit / ESP32-S31，序列号 `1c:29:04:d0:af:5e`。写入前核对设备身份和原分区表，只更新 `0x10000` 应用固件区，写入后通过哈希校验；保留现有应用版本、SD 数据和 NVS 分区。随后分别设置 35%、0%、100%、65%，收到同步音量日志后立即发送重启指令，四次开机均恢复对应数值。测试没有在音量变化后等待一秒轮询。见 [device.json](doit-gamekit-volume-20261019/device.json) 和经过过滤的 [volume-boot.log](doit-gamekit-volume-20261019/volume-boot.log)。

当前设备音量为 **65%**。测试前未准确采集用户原先的音量，因此 65% 是本次测试的结束值。早期开机控制台为 115200 波特，PXADB 就绪后切换 2M；最终验证分别采集两个阶段。原始启动记录保留在本地，提交证据仅包含音量相关行。

复现宿主检查：

```sh
python3 firmware/components/pxa_integration/tests/test_volume_preferences.py
python3 firmware/components/pxa_integration/tests/test_startup.py
```
