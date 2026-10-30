# 非标准扩展与 pxa-ext SDK

板级功能先检查当前版本的 PXA 标准服务及 Guest SDK：文件与 SD 使用 Storage/FS，
声音与麦克风使用 Audio，已定义的物理量使用 Sensor，振动使用 Haptics。
板型没有对应硬件时返回不可用或空枚举，不能为兼容应用伪造传感器或支持声明。

标准没有的功能优先复用本工作区 `extensions/` 中的扩展与配套 `pxa-ext` SDK。
没有合适的现有扩展时，定义独立的合同、UUID、版本协商、权限、消息与资源上限、
取消和组件停止清理规则，并登记到 `extensions/registry.json`。注册表只表示本
工作区的分配，不代表全局保留或 PXA 标准服务编号。Host 与 SDK 必须属于同一
合同；在 HELLO 身份与版本协商成功前，不能使用扩展资源。

| 扩展 | 用途 | C++ 头文件 / CMake 目标 |
| --- | --- | --- |
| `org.doit.voice-trigger` | pai-touch 语音触发控制 | `pxa-ext/voice_trigger.hpp` / `pxa::voice_trigger` |
| `org.doit.peripherals` | 逻辑 GPIO、I²C、UART、模块槽发现、RGB 指示灯、JPEG 与板内推理流 | `pxa-ext/peripherals.hpp`、`pxa-ext/vision.hpp` / `pxa::peripherals` |

协议和限制分别见 `extensions/voice-trigger/README.md` 与
`extensions/peripherals/README.md`。外设扩展只公开板子明确枚举的逻辑端口。
不得将电源轨、原始 Flash、EEPROM 配置或板内共享设备寄存器暴露为应用裸接口。
模块槽识别不代表该模块的专用驱动已经实现。

外设扩展 0.1 提供 GPIO/I²C/UART/模块槽，0.2 增加 RGB，0.3 增加视觉流；
旧客户端只枚举其协商版本支持的端口。Watcher 的 `camera.jpeg` 和
`vision.inference` 分别申请权限，后者只返回板内模型的推理 JSON；Korvo
提供 `camera.jpeg`。视觉流使用有界的 PXVF 帧和 SDK `FrameReader`，后台、
撤权或关闭时释放采集资源。端口存在不保证已有可用模型，应用必须处理失败。
`extensions/peripherals/examples/camera` 演示授权、分段读取和原子保存，
`examples/indicator` 演示指示灯与后台清理；完整合同以扩展 README 为准。

## 独立 SDK 使用

DevKit 与独立 SDK 资产将工作区扩展放在 SDK 根目录的 `extensions/`，保留其
独立命名空间及许可证。应用在 CMake 中包含所需扩展入口，再链接接口目标：

```cmake
# PXA_CPP_SDK_DIR 由 DevKit 的 C++ 应用构建设置。
include("${PXA_CPP_SDK_DIR}/../../extensions/peripherals/sdk/Peripherals.cmake")
target_link_libraries(my_component PRIVATE pxa::peripherals)
```

`my_component` 应替换为应用实际组件目标。扩展不会因随 SDK 分发而成为标准服务。
应用应查询可用性、处理身份不匹配/权限拒绝及缺少硬件的情况；仅在确实无法降级
时才将其作为必需条件。不要通过修改标准 SDK 头文件声明一个尚不存在的接口。

## 后续标准化

为新扩展保留公开协议和示例，必要时在 PXA 标准仓库提交 issue，附使用场景、
身份、版本和权限/资源模型。当未来标准覆盖该功能时，优先改用标准服务并说明
迁移方式；已有扩展仍按其版本处理兼容性，不将同一编号静默改作其他语义。
