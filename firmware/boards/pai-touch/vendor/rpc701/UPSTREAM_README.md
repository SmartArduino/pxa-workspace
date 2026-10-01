# rpc_701 — 基于 Protobuf 的 UART RPC 通信组件

## 概述

rpc_701 是一个面向嵌入式双芯片架构的 RPC 通信组件，运行在 ESP-IDF 环境中。主端（ESP32）通过 UART 与从端（VB701 音频蓝牙芯片）通信，实现蓝牙控制、音频传输、音乐播放等功能。

通信协议基于 **protobuf-c** 序列化，传输层使用自定义的串口帧格式（`0xAA55` 头 + 类型 + 长度 + 数据）。

## 架构

```
┌─────────────────────────────────────────────────┐
│                  应用层 (rpc_wrap)               │
│  rpc_vb_init / rpc_emitter_connect / rpc_heartbeat ...  │
├─────────────────────────────────────────────────┤
│              RPC 核心 (rpc_701)                  │
│  请求组装 ──→ protobuf 编码 ──→ 串口发送         │
│  串口接收 ──→ protobuf 解码 ──→ 响应/事件分发     │
├──────────────┬──────────────┬───────────────────┤
│  rpc_req.c   │  rpc_rsp.c   │   rpc_evt.c       │
│  请求组装     │  响应解析     │   事件解析         │
├──────────────┴──────────────┴───────────────────┤
│            rpc_slave_if (底层发送入口)            │
├─────────────────────────────────────────────────┤
│            rpc_transfer (串口帧传输层)            │
├─────────────────────────────────────────────────┤
│               UART 硬件                          │
└─────────────────────────────────────────────────┘
```

## 消息类型

| 类型 | ID 范围 | 方向 | 说明 |
|------|---------|------|------|
| Request (Req) | 256–279 (0x100+) | 主端 → 从端 | 命令请求 |
| Response (Resp) | 512–535 (0x200+) | 从端 → 主端 | 请求响应，ID 与 Req 一一对应 |
| Event (Evt) | 768–780 (0x300+) | 从端 → 主端 | 异步事件通知 |

**ID 规则**: `Resp_ID = Req_ID - 256 + 512`，protobuf oneof field number 等于 RpcId 值。

## 通信模式

- **同步请求**: 调用后阻塞等待响应（信号量 + 队列），默认超时 5 秒
- **异步请求**: 设置 `rpc_rsp_cb` 回调，发送后立即返回
- **事件**: 从端主动上报，通过注册的回调函数 + ESP 事件系统分发

## 已支持的 RPC 接口

### 请求 (Request)

| 接口 | 功能 |
|------|------|
| `rpc_vb_init` | 初始化从端配置（保活、功放引脚、编解码格式、蓝牙名称等） |
| `rpc_emitter_connect` | 以 emitter 模式连接蓝牙设备 |
| `rpc_emitter_start_scan` | 启动蓝牙扫描 |
| `rpc_get_wake_word_list` | 获取从端支持的唤醒词列表 |
| `rpc_asr_mic_mode_change` | 切换 ASR 麦克风模式（AEC/ANS） |
| `rpc_sd_music_count` | 查询存储设备音乐文件数量 |
| `rpc_sd_music_list` | 获取存储设备音乐文件列表 |
| `rpc_music_mode_get/set` | 获取/设置音乐源（蓝牙/SD/USB/Flash） |
| `rpc_music_track_switch` | 切换上一首/下一首 |
| `rpc_music_play_ctrl` | 播放控制（播放/暂停/切换） |
| `rpc_music_play_by_index` | 按索引播放指定曲目 |
| `rpc_music_is_playing` | 查询播放状态 |
| `rpc_music_loop_mode_get/set` | 获取/设置循环模式（顺序/单曲/随机） |
| `rpc_music_volume_get/set` | 获取/设置音量 |
| `rpc_power_off` | 关机 |
| `rpc_heartbeat` | 心跳检测 |
| `rpc_sd_card_status` | 查询 SD 卡插入状态 |
| `rpc_bt_status` | 查询蓝牙连接状态 |
| `rpc_time_get/set` | 获取/设置从端时间 |
| `rpc_sen_audio_pcm` | 发送 PCM 音频数据 |

### 事件 (Event)

| 事件 | 功能 |
|------|------|
| `VB_EVT_BT_CONNECTED/DISCONNECTED` | 蓝牙连接/断开 |
| `VB_EVT_BT_MUSIC_PLAY/PAUSE` | 蓝牙音乐播放/暂停 |
| `VB_EVT_VOLUME_CHANGED` | 音量变化 |
| `VB_EVT_MUSIC_INFO_TITLE/LYRC/TIME` | 音乐信息（标题/歌词/时间） |
| `VB_EVT_MUSIC_MODE_CHANGED` | 音乐源切换 |
| `VB_EVT_ASR_WORD` | 语音唤醒词识别 |
| `VB_EVT_EMITTER_SCAN_RESULT` | 蓝牙扫描结果 |
| `VB_EVT_STORAGE_DEV_CHANGED` | 存储设备插拔 |

## 文件说明

| 文件 | 作用 |
|------|------|
| `protobuf/rpc_messages.proto` | Protobuf 消息定义（枚举 + 结构体） |
| `protobuf/rpc_messages.pb-c.h/.c` | protoc-c 自动生成，勿手动编辑 |
| `protobuf-c/protobuf-c.c/.h` | protobuf-c 运行时库 |
| `rpc_701.h/.c` | 组件入口，初始化/反初始化、收发任务、同步/异步事务管理 |
| `rpc_transfer.h/.c` | 串口帧传输层（组帧、解帧、CRC） |
| `rpc_slave_if.h/.c` | 底层发送入口（每个 Req 一个发送函数） |
| `rpc_req.c` | 请求组装（应用结构体 → protobuf） |
| `rpc_rsp.c` | 响应解析（protobuf → 应用结构体） |
| `rpc_evt.c` | 事件解析（protobuf → 应用结构体） |
| `rpc_wrap.h/.c` | 高层 API 声明与实现、ESP 事件回调注册 |
| `port_esp_os.h/.c` | FreeRTOS 操作系统抽象层（队列、信号量、线程） |

## 使用方式

### 初始化

```c
#include "rpc_701.h"
#include "rpc_wrap.h"

// 1. 初始化 RPC 组件，传入 UART 写回调
rpc_701_config_t config = {
    .write_cb = my_uart_write_func,
};
rpc_701_init(&config);

// 2. 注册事件回调（桥接到 ESP 事件系统）
rpc_resister_event_callbacks();

// 3. 初始化从端配置
vb_init_conf_t conf = {
    .keep_alive_ms = 3000,
    .pa_io = 6,
    .pa_en_level = 1,
    .coder_fmt = CODEC_OPUS_20MS,
    .decoder_fmt = CODEC_OPUS_20MS,
    .debug_uart_io = -1,
};
strncpy(conf.bt_name, "MyDevice", sizeof(conf.bt_name));
rpc_vb_init(&conf);
```

### 发送请求

```c
// 同步请求示例：设置音量
int ret = rpc_music_volume_set(75);
if (ret != RPC_ERR_SUCCESS) {
    ESP_LOGE(TAG, "设置音量失败: %d", ret);
}
```

### 监听事件

```c
// 注册 ESP 事件处理器
esp_event_handler_register_with(loop, RPC_VB_EVENT, VB_EVT_VOLUME_CHANGED,
                                 volume_handler, NULL);

static void volume_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data) {
    event_volume_changed_t *evt = (event_volume_changed_t *)data;
    ESP_LOGI(TAG, "音量: %d", evt->volume);
}
```

### 音频传输

```c
// 发送 PCM 音频到从端
rpc_sen_audio_pcm(pcm_buffer, pcm_len);

// 读取从端上报的 OPUS 音频帧（内部 ringbuffer，满时自动丢弃旧帧）
uint8_t opus_buf[256];
int len = audio_opus_frame_read(opus_buf, sizeof(opus_buf));
```

## 串口帧格式

```
┌───────┬──────┬───────┬──────────┬─────┐
│ Header│ Type │ Length │   Data   │ CRC │
│ 2B    │ 1B   │ 2B     │ N bytes  │ 1B  │
│ 0xAA55│      │        │          │     │
└───────┴──────┴───────┴──────────┴─────┘
```

- **Type**: 0=RPC, 1=Audio, 2=AudioOPUS
- **最大数据长度**: 1024 字节

## 依赖

- ESP-IDF v5.4+
- FreeRTOS
- protobuf-c（已内置）
- ESP 事件循环 (`esp_event`)
- ESP Ringbuf (`esp_ringbuf`)

## 添加新消息

详见 `.claude/skills/rpc-guide/SKILL.md` 或 `.github/skills/rpc-guide/SKILL.md`，包含完整的 Event 和 Request 添加步骤。
