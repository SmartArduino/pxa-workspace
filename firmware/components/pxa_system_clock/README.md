# PXA 系统日历时间

状态栏、锁屏和通知面板统一读取这个服务生成的时间状态。网络尚未同步时也有
可用的日期和时间，使用系统时钟持续走时；联网校时仍由各板卡原有的 SNTP 实现负责。

启动时依次尝试：

1. 板卡注册的外置 RTC、芯片 RTC 或其他芯片的时间。
2. 系统已经保留的有效时间，包括重启保留时间、深睡恢复时间或已同步时间。
3. NVS `pxa_clock/utc` 中上次关机或重启保存的 UTC Unix 秒数。
4. 默认本地时间 **2026-01-01 00:00:00**，按产品配置的 `TZ` 转成 UTC。

首次启动的初始状态也填入时间，避免板卡第一次状态定时器触发前显示占位符。
`time_valid` / `date_valid` 表示时间可显示，不表示已通过网络校准。
服务只接受 2020-01-01 至 9999-12-30 UTC 范围内的数据；RTC 未设置、振荡器异常、
读取失败或数据损坏时应返回 `false`，系统会继续尝试后面的来源。

## 板卡接入

`pxa_integration_start()` 在 NVS、时区和板卡初始化之后、系统 UI 创建之前调用
`pxa_system_clock_initialize()`。板卡的初始状态和周期性 `PublishStatus()` 调用
`pxa_system_clock_update_status(&status)`，它只更新日期和时间字段。

`esp_restart()` 通过 shutdown handler 自动保存。真正切断电源、关闭 RTC 总线、
卸载存储或进入深睡之前，板卡须在任务上下文显式调用 `pxa_system_clock_save()`。
Pai Touch、SenseCAP Watcher 和 ESP Mosaico 的软件关机路径已接入。
不会周期性写入 Flash；无独立供电 RTC 时，关机期间无法计时，开机从保存的时刻继续。
突然断电、硬复位和 panic 不保证执行保存回调。

## 替换时间来源

在板卡 `initialize` 中初始化对应硬件并注册回调；服务会复制描述符，`context`
指向的驱动对象必须保持有效。所有回调使用 **UTC Unix 秒数**，硬件寄存器的日历、
BCD、时区和有效性检测由板级驱动转换，UI 不需要修改。

```cpp
#include <pxa_system_clock.h>

// rtc 对象的 ReadUtc/WriteUtc 由该板卡驱动实现。
pxa_system_clock_source_t source = {};
source.struct_size = sizeof(source);
source.context = &rtc;
source.read_utc = [](void* context, int64_t* seconds) {
    return static_cast<BoardRtc*>(context)->ReadUtc(seconds);
};
source.write_utc = [](void* context, int64_t seconds) {
    return static_cast<BoardRtc*>(context)->WriteUtc(seconds);
};
if (!pxa_system_clock_set_source(&source)) return false;
```

`read_utc` 在启动时读取硬件并初始化系统时钟，之后由系统时钟走时。
`write_utc` 在保存时接收当前系统时间，包括网络或手动校正后的结果，适用于可写 RTC
或协处理器；只读时间源可将该回调设为 `NULL`。未注册来源时直接使用系统 RTC + NVS。
芯片 RTC 若已由平台恢复至 libc 系统时间，无须额外注册。时间来源仅允许在服务初始化前
配置；重复初始化不会倒退正在走动的时间。

保存会分别尝试 RTC 写回和 NVS 提交，其中一个失败也会尝试另一个；任一必要写入失败
时返回 `false` 并记录日志。对已注册的硬件回调须确保重启时驱动和供电仍可用。

## 验证

```sh
bash firmware/components/pxa_system_clock/tests/test_host.sh
python3 firmware/components/pxa_integration/tests/test_startup.py
# 已构建 pai-touch 模拟器时，验证真实 LVGL 状态栏、通知面板和锁屏：
python3 firmware/components/pxa_system_clock/tests/test_ui.py
```

主机测试使用生产服务源码和可控的系统时间、NVS、RTC 边界，覆盖默认时间走动、
UTC 保存恢复、重启回调、时区切换、来源优先级、RTC 断电期间继续走时、
网络/手动校正后的写回、损坏数据和读写失败。
