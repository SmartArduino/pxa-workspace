# PXA 音频接口与实现

## 当前能播放什么

| 输入 | Pai Touch / Sensecap Watcher | 桌面模拟器 |
| --- | --- | --- |
| PCM 流 | 16 kHz、单声道、signed 16-bit little-endian | 相同 |
| 合成音 | sine / square / triangle / noise，带延迟、attack、release | 相同混音代码 |
| 预加载 `.pcm` 音效 | unsigned 8-bit、16 kHz、单声道，1–16,000 字节，不循环；Assets 句柄播放 | 相同 |
| `.ogg` 音乐 | Ogg Vorbis / Opus，流式解码 | Ogg Vorbis / Opus，流式解码 |
| MP3 / WAV / 录音 | 未提供该 Guest 接口 | 未提供该 Guest 接口 |

音乐输入接受 8–96 kHz、单声道或双声道，转为 16 kHz 单声道输出。ESP 使用线性插值重采样；桌面使用 SDL 重采样，不能据此保证两者音质完全相同。跨平台资源建议使用 **16 kHz 单声道 Ogg Vorbis**。原始 `.pcm` 文件与 PCM 流的位深、有符号性不同，不能混用。

每个应用 Host 最多有 3 个会话。PCM/合成音每会话独立 FIFO，每个 FIFO 最多 4 个命令，每次 PCM 写入不超过 320 个采样（640 字节，20 ms）。三个会话同时混音；同一会话的 PCM 和合成音按提交顺序串接。延迟是该 FIFO 中的相对延迟，不是绝对时间。需要重叠的合成音使用不同会话。

资源总线支持一条背景音乐和最多 6 个同时播放的短音效。另一个会话占用背景音乐时返回 `WOULD_BLOCK`。同一会话播放新音乐会替换自己的旧音乐；短音效可以重叠。短音效复用 Assets 的有界共享缓存，不再维护音频私有的八项 PCM 路径缓存。Guest 句柄、播放中的声音和缓存各有独立引用；停止声音后，无其他消费者的缓存对象可由资源 worker 淘汰。

## Guest SDK 用法

使用 `deps/pxa-system/sdk/guest-c/include/pxa_audio.h`。在应用清单声明 `audio.playback` 权限，申请 `media` scope。流程是：

1. 获得权限句柄后调用 `pxa_audio_open_media(token, permission)`。
2. 等待对应结果，用 `pxa_audio_parse_open` 取得会话句柄和输出格式。
3. 调用 `pxa_audio_commit_gain(token, session, gain_db_q8)`，或 `commit_graph` 设置 gain 和 EQ；等待成功结果。
4. 使用直接 I/O 写 PCM、发合成音命令或播放文件。
5. 正常事件回调中不再使用时，用 `pxa_close_handle` 关闭会话。应用最终 stop 回调禁止 imports，由 Core 自动撤销句柄与请求。

新增简便接口避免每次手写协议包或 scratch buffer：

```c
// 已获得 session，并且 commit 成功后：
pxa_audio_beep(session, 880, 80, -12 * 256);
uint64_t music_instance = 0;
int32_t accepted = pxa_audio_play_music(
    session, "assets/bgm.ogg", 1, -12 * 256, &music_instance);
// accepted > 0 仅代表已接受；保存 music_instance，在事件中匹配 READY 和终态。
pxa_audio_control_asset(session, PXA_AUDIO_ASSET_PAUSE, 0);
pxa_audio_control_asset(session, PXA_AUDIO_ASSET_RESUME, 0);

// 以下仍是异步请求；token 用来匹配结果。
pxa_audio_query(query_token, session);
pxa_audio_flush(flush_token, session);
```

`play_music` 使用 528 字节栈缓冲，路径最大 511 字节。旧 `play_file` / `play_asset` 仍复用同一 Ogg 引擎，但不返回播放实例编号。需要处理就绪、结束和错误时使用 Audio 0.7 的 `play_music`。资源应放在包的 `assets/` 下，并纳入签名清单。

桌面与 ESP 运行时信任安装阶段的校验结果，不重复做资源 SHA，也不检测安装后修改。PXRI 1.3 不再包含音乐或地图的块摘要。音乐直接读入解码器输入缓冲，没有专用的 4 KiB 校验缓冲和块内拷贝；路径、类型、长度、预算和取消检查继续保留。排队/当前播放持有目录引用；退出先停止音频，再释放目录。桌面共享存储测试已覆盖供给充足、3 秒突发停顿、低预算恢复和停顿中退出；ESP 的生产读取入口也已加入分块仲裁：排队中的音乐先于下一块纹理读取，复用已有 worker/decoder 的任务通知。主机真实文件测试覆盖优先级、取消、停顿中退出和读取错误后的释放；已发出的真实 OS read 仍须完成，设备带宽与最长读取时间需要真机测量。

Audio 0.6 的 `play_file` 只处理 Ogg。短 PCM 改用 Assets 2.0，应用需声明 Assets 服务并重新打包：

```c
// 加载阶段；返回 0 表示已接受，之后由已有事件分发器接收结果。
pxa_assets_load_sound(load_token, "assets/sfx/hit.pcm");

// 对应 LOAD 事件中解析，成功后保存 result.handle。
pxa_asset_result_t result;
if (pxa_assets_parse_result(&event, load_token, PXA_ASSETS_LOAD, &result)
    && result.status == 0) sound = result.handle;

// 已就绪的音效可多次触发。返回 12 表示接受，无文件 I/O 或 PCM 复制。
pxa_audio_play_sound(session, sound, -6 * 256);
```

以上片段分别属于加载、事件处理与播放阶段，需要包含 `pxa_assets.h` 和音频 SDK。未接受的加载请求不会产生完成事件；已接受请求的取消与晚到成功沿用 Core 规则。关闭音效句柄不会截断已接受的声音，停止使用音频控制操作。播放声部满返回 `WOULD_BLOCK`，由应用决定稍后重试或舍弃过时音效。

增益单位是 dB × 256。PCM/合成音 graph 支持 -48 到 +12 dB、最多 5 个 peaking EQ，中心频率必须低于输出 Nyquist（8 kHz）。graph 的增益和 EQ 作用于 **PCM/合成音总线**；文件播放使用 `play_file` / `SET_GAIN` 的资源增益，当前不通过此 EQ。不要把它当作整个应用的 master graph。设备音量属于设备层。

直接 I/O 返回已接受的命令字节数或负状态码。`WOULD_BLOCK` 表示未接受，不会偷偷清除旧音频；保留该数据，等下一次应用 tick 再重试，避免忙等。PCM 写入必须按 open 返回的格式分帧，不能把整个歌曲一次传入。Query 的 submitted/accepted/queued 只统计 PCM/合成音，不包括文件解码；accepted 指交给输出端，不代表扬声器已经播放完毕。

播放文件返回成功代表命令被接受。Audio 0.7 通过 `PXA_AUDIO_PLAYBACK_EVENT` 发送 READY、ENDED、STOPPED、REPLACED、ERROR，用 `pxa_audio_parse_playback` 解析并同时匹配 `session` 与 `instance`。READY 后才适合开始淡入；ERROR 带负状态码。换曲时旧实例的晚到通知不能修改新曲状态；命令被同步拒绝时旧曲保持不变。

桌面与 ESP 使用同一个 `pxa_audio_buffer` 状态机。沿用 8192 个单声道 S16 样本的环形缓冲（16 KiB），准备好 4096 个样本后开始输出并发送 READY。4096 样本对应 16 kHz 下的 256 ms 音频数据，解码填充完成即可输出。短曲到 EOF 时允许不足阈值的尾段播放，避免一直等到无法达到的阈值。

一次取样不足计为一次欠载，并进入重新缓冲；缓冲期间累计缺失样本，补充到阈值或 EOF 尾段后恢复。恢复不重复发送 READY，不申请更大的 ring。暂停期间不计欠载；正常 EOF 不计欠载，最后的输出调用完成后才报告 ENDED。

桌面日志 `PXA MUSIC BUFFER` 记录欠载/恢复、缺失及消费样本、高低水位和接受到 READY 的最大时间。低水位只在非 EOF 的有效取样后记录；`low_water_valid=0` 表示尚未取到有效观测。`PXA MUSIC DECODE` 是解码调用墙钟时间减去输入回调时间，仍包含线程被调度器暂停的时间，不能当作纯 CPU 周期。`PXA STORAGE` 分别记录音乐与资源的读取量、排队及服务时间。ESP 的 `PxaAudioOutput::GetMusicStats` 和曲目退出时的日志提供设备累计计数，读取与解码分别计时；这些指标反映 Host PCM 队列，不等同于 I2S/DAC 欠载测量。

每个实例最多一个 READY 和一个终态，准备前停止或失败可以只有终态。Host 使用八项固定邮箱保存通知，Core 队列满时保留待投递记录；邮箱耗尽拒绝新播放，音频线程不等待 Guest。关闭会话会撤销其待投递通知。ENDED 等待 Host ring 和正在执行的输出调用完成，不能据此断言 SDL、远端 codec 或 DAC 已经物理播放完最后一个采样。

完整事件用法见 `local/pxa-apps/resource-scenes/main.c` 与 Pixel Dungeon 的 `audio.c`。前者在对应实例 READY 后推进场景，在 STOPPED 后清理退出；后者等待 READY 淡入，并忽略旧曲通知。

## 暂停、停止和生命周期

后台/锁屏时 Host 暂停音频。PCM 和合成音保留队列、相位和滤波状态，恢复后接着播放；短音效、音乐都按会话暂停。Guest 主动暂停的文件，不会因回到前台而自动恢复。

`CONTROL_ASSET STOP` 只停止该会话的音乐和短音效。`FLUSH` 清除该会话待播 PCM/合成音和文件；即使 FIFO 已满也可执行。关闭会话不清除其他会话的音频。已经混入物理输出队列或设备 DMA 的采样无法按会话拆除，可能仍有短暂尾音；FLUSH 不是硬件 DAC 的精确停止屏障。

## 实现与性能边界

- `libpxa/src/services/audio/audio_mixer.c` 是桌面、ESP 共用的无分配混音核心。固定 workspace 为 **8,640 字节**，含 3×4 命令、计数器和 EQ 状态。正弦使用查表插值；EQ 系数只在 commit 时计算。
- ESP 有音频时每 20 ms 混合一帧，空闲时等待任务通知；输出和解码线程空闲时等待命令，短时间 mutex 保护状态，不在关中断的临界区里运行滤波。输出队列满时回滚采样、噪声相位和 EQ 状态，下次重试，避免静默丢音。
- 共用 `PxaAudioOutput` 负责资源、解码、混合文件音频和物理输出；板级驱动只负责 RPC701 / ES8311。移除了 Pai 对完整 GMF pipeline 的依赖，直接使用 codec 包。
- 桌面与 ESP 的音乐 PCM ring 都固定为 **8,192 个采样，即 16 KiB**。歌曲长度不影响该缓冲大小。解码器本身、线程栈、SDL converter 仍需额外内存，16 KiB 不是整个音频系统的 RAM 用量。
- ESP 音乐输入仅保留文件位置、目录引用和取消状态；已去掉 4096 B 校验缓冲及 mbedTLS 哈希上下文。排队与当前输入都计入 TEMP，替换准入期间可以重叠。固定命令元素为 16 B。实际两板的新对象大小在完成固件构建后重新记录。
- ESP 解码输出缓冲按需分配，从 16 KiB 增长，最大 64 KiB；停止/播放结束后释放。短音效冷读取在现有资源 worker 上完成，数据直接进入最终共享对象，读取完成后发布。对象、加载峰值和闲置缓存共同受资源与应用预算限制，播放触发不执行冷读取。
- ESP 最终混音后的待输出队列由 12 帧降为 3 帧（约 60 ms）。实际端到端延迟还包括 Guest 排队、解码、RPC/I2S 和设备自己的缓冲。

Sensecap 新增了文件解码线程和缓冲，相比原来只输出 PCM 的版本会增加 RAM 用量。

这些是代码中的容量和行为约束，不是真机 CPU、延迟、功耗测量。当前没有连接 ESP 设备。

## 验证

```sh
bash firmware/components/pxa/tests/test_host.sh
cmake --build build/simulator/pai-touch --target pxsys_audio_test -j8
python3 deps/pxa-system/tools/package/test_audio_backend.py \
  build/simulator/pai-touch/pxsys_audio_test \
  deps/pxa-system/simulator/desktop/tests/audio-assets
```

混音核心测试在 libpxa 的 `pxa_audio_mixer_test` 中；覆盖饱和混音、短包与 FIFO 顺序、背压、长音调、包络、EQ 实际增益和非法频率。ESP 主机测试覆盖输出拒绝后重试、会话隔离、暂停相位保持、音效缓存淘汰和回收。SDL 测试使用 dummy 音频设备，验证真实桌面后端，并分别完整消费 Vorbis、Opus 测试 Ogg 的 16,000 个输出采样。

模拟器构建需要 SDL2、libvorbisfile 和 libopusfile 的开发包。测试文件 `tone.ogg`、`tone-opus.ogg` 都是生成的 1 秒 440 Hz 正弦：

```sh
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=16000:duration=1 \
  -c:a libvorbis tone.ogg
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 \
  -c:a libopus tone-opus.ogg
```
