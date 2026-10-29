# VoiceTrigger 0.1

可选的设备 Host 扩展，独立于 PXA 标准服务和默认 C++ SDK。协议及设备接入见
[接口说明](../../docs/voice-trigger.zh-CN.md)。工作区分配登记在
[`extensions/registry.json`](../registry.json)，不是全局保留的厂商编号段。

应用构建时单独引入 SDK：

```cmake
include("${PXA_VOICE_TRIGGER_ROOT}/sdk/VoiceTrigger.cmake")
target_link_libraries(my_app PRIVATE pxa::voice_trigger)
```

```cpp
#include <pxa-ext/voice_trigger.hpp>
namespace voice = pxa::ext::voice_trigger;
// 协程中先 open(ctx)，查询 capabilities/list_profiles/list_keywords，
// 取得明确授权后 subscribe(profile, permission)。不支持时保留触摸/按键入口。
```

使用当前工作区 C++ SDK 的通用 `Context::request(..., close_late_handle)`，
使资源返回请求取消后到达的成功句柄也能关闭；没有新增核心语音接口。
Host 使用 `include/pxa_ext/voice_trigger.h` 的后端与服务接口，编译
`src/voice_trigger.c` 并链接 libpxa。0.1 只提供默认配置的只读词表与前台检测订阅，
不提供训练、语法更新、模型导入、后台助手或唤醒音频缓存。

本机测试（输出目录按仓库约定放在 `local/artifacts/`）：

```sh
cmake -S extensions/voice-trigger -B "$artifact_dir/host-build" \
  -DCMAKE_CXX_COMPILER=clang++ -DPXA_BUILD_TESTS=OFF -DPXA_BUILD_ADAPTERS=OFF
cmake --build "$artifact_dir/host-build"
ctest --test-dir "$artifact_dir/host-build" --output-on-failure
```

`tests/build_simulator_probe.py --build <已配置的模拟器构建目录> --artifacts <本地产物目录>`
生成带模拟后端的真实 PXA 模拟器。它通过测试泵注册扩展，不修改标准模拟器。
小智的 `tests/simulator.py --scenario voice-trigger|voice-wait` 可验证对话状态与生命周期；
`--ime-package` 仍需指向受信任的输入法包。模拟后端声明支持播放中检测只用于测试，
不构成对实际设备 AEC 或识别效果的验证。

许可：Apache-2.0，见 [LICENSE](LICENSE)。没有新增第三方运行库。
