# 可选 VoiceTrigger Host 扩展

VoiceTrigger 0.1 是工作区设备扩展，不是 PXA 标准必选服务。识别在设备 Host 后端完成，
应用只接收授权的触发事件，继续自行处理会话、服务器协议、音频采集及播放。
没有扩展的 Host 返回 `unsupported`，应用仍可安装并通过触摸、按键操作。

## 接入与版本

- 工作区分配：service ID / resource type 均为 `33025`（`0x8101`），定义在
  `extensions/registry.json`。此分配不表示 PXA 已定义厂商编号区间。
- 身份：`cd3479d1-53f1-47bc-a8cc-54873b706cb2`；协议 `0.1`。
- 独立 SDK：`extensions/voice-trigger/sdk/include/pxa-ext/voice_trigger.hpp`，
  通过 `sdk/VoiceTrigger.cmake` 引入，不加入基础 `Context` 的语音成员或默认 include。
- 应用不把扩展列入 Component 的强制 services 依赖；使用时运行时探测。
- 权限声明为可选的 `org.doit.voice-trigger`，scope 为 `system.default`。
  收到词语事件不授予麦克风采集权限，实际录音仍需 `audio.capture`。

Host 的权限 authority 容量需覆盖应用已声明的权限及并行 Component。不能假定应用
只持有四个音频/网络权限；语音触发或其他可选扩展需要独立的并存权限。

所有整数为小端，控制请求使用 PXA 标准 envelope 和非零 request token；响应使用
标准 `status:i32 + body`。当前协议使用 Core 2 的 64 位句柄。HELLO 校验身份和版本；
身份不匹配不能解释为支持同一协议。

## 控制接口

| Opcode | 请求 body | 成功响应 body |
| --- | --- | --- |
| 1 HELLO | 身份 16 字节、min major/minor、max major/minor（均 u16） | 身份 16 字节、major:u16、minor:u16 |
| 2 CAPABILITIES | 空 | flags:u32、ready:u32、profile_count:u16、max_subscriptions:u16、max_keywords:u16、max_word_bytes:u16、revision:u32、update_mode:u32 |
| 3 LIST_PROFILES | 空 | count:u16，后接配置记录 |
| 4 LIST_KEYWORDS | profile_id:u32、revision:u32 | revision:u32、count:u16、reserved:u16=0，后接词语记录 |
| 5 SUBSCRIBE | profile_id:u32、revision:u32、permission_handle:u64 | subscription:u64、revision:u32 |

配置记录为 `id:u32、revision:u32、keyword_count:u16、key_bytes:u16、UTF-8 key`。
词语记录为 `id:u32、label_bytes:u16、UTF-8 label`。禁止重复 ID、重复文本、空文本、
嵌入 NUL 和不完整 UTF-8；0.1 最多 16 个唤醒词，每个最多 63 字节。
版本不一致返回 `bad_state`，应用需重新查询词表，不能把旧 ID 映射到新词语。

0.1 提供一个稳定配置：ID `1`，key 为 `system.default`。词语 ID 只在对应 revision
中有效。配置变化不能要求应用把检测词硬编码到程序内。

`flags` 的 bit 0 为前台使用限制，始终置位；bit 1 表示后端声明支持播放中检测。
清除 bit 1 时不保证播放中的识别效果，不能把存在硬件事件等同于已经验证 AEC。
0.1 的 `update_mode=0`，词表只读，不提供 `setWakeWord(string)`。语法包、设备绑定
加密资源或训练模型需要独立的配置管理权限与事务协议；实现后另行协商新版本，
不能仅修改显示名称来宣称已经修改识别词。

## 事件与生命周期

`service=33025, opcode=0x8001, token=0`，固定 40 字节 payload：

```text
subscription:u64 | sequence:u64 | timestamp_us:u64 |
revision:u32 | keyword_id:u32 | kind:u32 | status:i32
```

`kind=1` 是触发事件，status 为 0；`kind=2` 是检测失败，keyword_id 为 0，status 为
标准负错误码。时间使用 Host 单调时钟。触发序号在一次订阅内单调递增，SDK 校验完整
句柄代际、revision 和序号。失效订阅的旧事件不能唤醒新会话。

事件以 reliable、无合并键发送；Host 邮箱暂满时保留一个待发送事件并重试，后端队列
也必须有界。两秒前的触发转换为 `timed_out` 故障，不延迟执行旧唤醒。队列溢出必须
报告错误，不静默覆盖。后端丢失或故障后停止检测，应用关闭订阅并按需重新探测。

订阅是绑定权限 authority 的 RAII 资源；关闭、撤权及 Component 停止会停止检测。
Host 必须在失去前台或屏幕不再交互时关闭资源，不能只依赖应用主动取消。
0.1 一次只允许一个前台订阅，其他请求返回 `busy`，不实现后台多助手仲裁。

## Host 后端

`pxa_vt_backend_t` 包含 `profile/start/read/stop`。回调在 runtime owner 线程执行，
不得阻塞；串口事件只复制数据到有界队列，由 owner 线程轮询派送。`start` 校验配置
revision 并清空旧事件；`stop` 关闭转发并清空队列。Host 在订阅期间至少每 20 ms
轮询服务。服务工作区与 runtime 同寿命，所有资源关闭后才能释放。

pai-touch 默认启用扩展，可在固件构建时通过 `-DPXA_ENABLE_VOICE_TRIGGER=OFF`
完全移除实现。其他板型默认不启用。板级代码通过
`pxa_esp_voice_trigger_set_backend()` 注册，不需要修改 PXA Board API 或标准服务表。

JL701 后端通过单次后台任务查询设备词表，失败有限频率重试，不阻塞心跳、播放或
Guest。只接受 `WORD_TYPE_WAKEUP`，忽略命令词；对 Ex 事件按芯片序号去重，并对旧式
事件和重复格式做短时间去重。芯片重新建立连接后重新读取词表。词表、事件队列和
工作线程均由板级代码拥有，不把 JL701 RPC 或 FST 格式暴露给 Guest。

当前后端不切换 ASR 麦克风模式，不宣称播放中识别或预录音已验证。软件识别后端需要
统一的 Host 音频入口及分发，不能与应用各自消费同一个破坏性采集队列；是否引入
AEC、播放参考与唤醒缓存，应由对应后端的能力和实际验证决定。

## 小智的使用策略

“对话”设置里的语音唤醒默认关闭；开启后申请独立权限。支持时显示设备返回的实际
唤醒词，扩展缺失或后端未就绪时保留普通对话。撤销此权限不关闭正在进行的网络或
录音会话，进入设置、后台时停止订阅，恢复前台后重新建立订阅。

- 待机收到触发：重新连接并开始聆听。
- 已在聆听：保持当前聆听，不能复用点击的待机切换逻辑。
- 思考或播放中收到触发：取消旧回复，重新聆听；思考取消使用新连接隔离旧回复。
- 未联网、未验证：显示对应提示，不误显示为正在思考。
- 设置及后台：忽略触发，不抢走当前操作。

小智服务器的 `abort` / `listen` 由应用处理。唤醒导致的 abort 使用
`reason=wake_word_detected`，参见[小智 WebSocket 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md)。
