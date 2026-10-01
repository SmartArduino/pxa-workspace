#ifndef _RPC_WRAP_H_
#define _RPC_WRAP_H_

#include "stdint.h"
#include "esp_event.h"
#include "rpc_slave_if.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(RPC_VB_EVENT);

/**
  * @brief ESP-Hosted event declarations
  */
enum {
	VB_EVT_BT_CONNECTED = 0,
    VB_EVT_BT_DISCONNECTED,
    VB_EVT_BT_MUSIC_PLAY,
    VB_EVT_BT_MUSIC_PAUSE,
    VB_EVT_VOLUME_CHANGED,
    VB_EVT_MUSIC_INFO_TITLE,
    VB_EVT_MUSIC_INFO_LYRC,
    VB_EVT_MUSIC_INFO_TIME,
    VB_EVT_MUSIC_MODE_CHANGED,
    VB_EVT_ASR_WORD,
    VB_EVT_EMITTER_SCAN_RESULT,
    VB_EVT_STORAGE_DEV_CHANGED,
    VB_EVT_HFP_STATUS,
    VB_EVT_SCO_STATUS,
    VB_EVT_DOA_ANGLE,
    VB_EVT_ALARM_FIRED,
    VB_EVT_BUTTON,
    VB_EVT_ASR_WORD_EX,
    /* 曲目列表缓存有新进展（负载 rpc_music_cache_status_t，见 rpc_music_cache.h）。
     * 不是 701 推的事件，由本组件的缓存层自己发。 */
    VB_EVT_MUSIC_LIST_UPDATED,
};

  typedef struct {
    size_t title_len;
    char title[];
  } rpc_vb_music_title_event_t;

  typedef struct {
    size_t lyrc_len;
    char lyrc[];
  } rpc_vb_music_lyrc_event_t;

  typedef struct {
    size_t name_len;
    uint8_t addr[6];
    uint32_t dev_class;
    int32_t rssi;
    char name[];
  } rpc_vb_emitter_scan_result_event_t;

  /**
   * @brief 初始化 VB RPC 封装层并下发基础配置。
   *
   * 该接口会将保活时间、功放控制引脚、电编解码格式、蓝牙名称
   * 以及调试串口等配置同步到 RPC 从端。
   *
   * @param conf 初始化配置，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_vb_init(vb_init_conf_t *conf);

  /**
   * @brief 以 emitter 模式连接指定蓝牙设备。
   *
   * @param addr 目标设备蓝牙地址，长度固定为 6 字节，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_emitter_connect(const uint8_t *addr);

  /**
   * @brief 获取从端支持的唤醒词列表。
   *
   * 成功时会填充 list 中的数量与内容；当 words_list 非空时，内存由该接口
   * 动态分配，使用完成后需要由调用方释放。
   *
   * @param list 输出唤醒词列表，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_get_wake_word_list(wake_word_list_t *list);

  /**
   * @brief 启动 emitter 设备扫描。
   *
   * 扫描结果会通过 RPC_VB_EVENT 事件基和 VB_EVT_EMITTER_SCAN_RESULT 事件上报。
   *
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_emitter_start_scan(void);

  /**
   * @brief 切换 ASR 麦克风处理模式并设置增益。
   *
   * @param mode 麦克风处理模式，例如 AEC 或 ANS。
   * @param gain 对应模式下使用的增益值。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_asr_mic_mode_change(asr_mic_mode_t mode, uint32_t gain);

  /**
    * @brief 获取存储设备（SD/USB）上的音乐文件数量。
    *
    * @param dev 存储设备类型（SD0, SD1 或 USB）。
    * @param total_out 输出音乐文件总数，需由调用方保证非空。
    * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
    */
  int rpc_sd_music_count(storage_dev_type_t dev, uint32_t *total_out);

    /**
   * @brief 获取存储设备（SD/USB）上的音乐文件列表。
   *
   * 该接口会从存储设备查询指定区间的音乐文件。成功时会填充响应结构中
   * 的文件列表；当 files 非空时，内存由该接口动态分配，使用完成后
   * 需要由调用方释放。
   *
   * @param dev 存储设备类型（SD0, SD1 或 USB）。
   * @param offset 起始文件索引 (0-based)。
   * @param count 请求的文件个数。
   * @param resp_out 输出响应数据，包含文件列表及总数，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_sd_music_list(storage_dev_type_t dev, uint32_t offset, uint32_t count, sd_music_list_resp_t *resp_out);

  /**
   * @brief 音乐列表异步回调。
   *
   * 在 RPC RX 任务上下文执行（超时/发送失败时在 esp_timer 任务），
   * 不得阻塞、不得再同步发 RPC；resp 及其 files 数组仅在回调调用期间
   * 有效，返回后由框架释放，需要的数据必须拷走，不得自行 free。
   *
   * @param result RPC_ERR_SUCCESS 或负数错误码（如 RPC_ERR_TIMEOUT）。
   * @param resp   成功时的列表数据；失败时为 NULL。
   * @param user_ctx 发起调用时传入的上下文原样带回。
   */
typedef void (*rpc_music_list_cb_t)(int result, const sd_music_list_resp_t *resp, void *user_ctx);

  /**
   * @brief 异步获取音乐文件列表：发出请求立即返回，结果经 cb 回调。
   *
   * 返回 RPC_ERR_SUCCESS 仅表示请求已入队，最终结果（含发送失败、
   * 超时）一定会且只会通过 cb 通知一次；返回负数错误码时 cb 不会被调用。
   *
   * @param dev 存储设备类型（SD0, SD1 或 USB）。
   * @param offset 起始文件索引 (0-based)。
   * @param count 请求的文件个数。
   * @param cb 结果回调，不可为 NULL，契约见 rpc_music_list_cb_t。
   * @param user_ctx 透传给 cb 的用户上下文，可为 NULL。
   * @return 入队成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_sd_music_list_async(storage_dev_type_t dev, uint32_t offset, uint32_t count,
                            rpc_music_list_cb_t cb, void *user_ctx);

  /**
   * @brief 获取当前音乐播放模式。
   *
   * @param mode_out 输出音乐模式，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_mode_get(music_mode_t *mode_out);

  /**
   * @brief 设置当前音乐播放模式。
   *
   * @param mode 目标音乐模式。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_mode_set(music_mode_t mode);

  /**
   * @brief 切换当前播放曲目。
   *
   * @param next 为 true 表示下一首，为 false 表示上一首。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_track_switch(bool next);

  /**
   * @brief 按索引播放存储设备中的音乐曲目。
   *
   * @param index 音乐文件索引 (0-based)。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_play_by_index(uint32_t index);

  /**
   * @brief 控制音乐播放状态。
   *
   * @param ctrl 播放控制命令，例如播放、暂停或切换。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_play_ctrl(music_play_ctrl_t ctrl);

  /**
   * @brief 查询当前是否正在播放音乐。
   *
   * @param is_playing_out 输出播放状态，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_is_playing(bool *is_playing_out);

  /**
   * @brief 设置当前音乐循环模式。
   *
   * @param loop_mode 目标循环模式。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_loop_mode_set(music_loop_mode_t loop_mode);

  /**
   * @brief 获取当前音乐循环模式。
   *
   * @param loop_mode_out 输出循环模式，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_loop_mode_get(music_loop_mode_t *loop_mode_out);

  /**
   * @brief 设置当前音乐音量。
   *
   * @param volume 目标音量值。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_volume_set(int32_t volume);

  /**
   * @brief 获取当前音乐音量。
   *
   * @param volume_out 输出音量值，需由调用方保证非空。
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_music_volume_get(int32_t *volume_out);

  /**
   * @brief 触发从端执行关机。
   *
   * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
   */
int rpc_power_off(void);

  /**
   * @brief 向从端发送一帧 PCM 音频数据。
   *
   * 该接口会按音频数据包类型直接透传缓冲区内容。
   *
   * @param data PCM 音频数据缓冲区。
   * @param len 缓冲区长度，单位为字节。
   * @return 成功返回 0。
   */
int rpc_sen_audio_pcm(uint8_t *data, size_t len);

  /**
  * @brief 向从端发送心跳请求，确认从端存活。
  *
  * 该接口为阻塞同步调用，超时时间约 3 秒，请勿在不可阻塞的上下文中调用。
  *
  * @return 成功返回 RPC_ERR_SUCCESS，超时或失败返回负数错误码。
  */
int rpc_heartbeat(void);

 /**
  * @brief 设置从端心跳保持时间。
  *
  * @param keep_alive_ms 目标心跳保持时间，单位毫秒。
  * @param applied_keep_alive_ms_out 可选输出实际应用值，可为 NULL。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_heartbeat_set(int32_t keep_alive_ms, int32_t *applied_keep_alive_ms_out);

 /**
  * @brief 请求从端重启。
  *
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_reboot(void);

  /**
  * @brief 查询 SD 卡是否插入。
  *
  * @param inserted_out 输出 SD 卡插入状态，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_sd_card_status(bool *inserted_out);

  /**
  * @brief 查询蓝牙是否已连接。
  *
  * @param connected_out 输出蓝牙连接状态，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_status(bool *connected_out);

  /**
  * @brief 获取从端的当前时间。
  *
  * @param time_out 输出时间信息，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_time_get(time_get_resp_t *time_out);

  /**
  * @brief 设置从端的当前时间。
  *
  * @param year 年
  * @param month 月
  * @param day 日
  * @param hour 时
  * @param minute 分
  * @param second 秒
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_time_set(uint32_t year, uint32_t month, uint32_t day,
                 uint32_t hour, uint32_t minute, uint32_t second);

  /**
  * @brief 注册 RPC 事件回调并桥接到 ESP 事件系统。
  *
   *
   * 注册成功后，蓝牙连接状态、音乐播放状态、音量变化、ASR 识别结果、
   * 扫描结果等事件会通过 RPC_VB_EVENT 对外分发。
   *
   * @return 成功返回 SUCCESS，失败返回 FAILURE。
   */
int rpc_resister_event_callbacks(void);

  /**
  * @brief 断开当前蓝牙连接，并解绑（删除配对信息）。
  *
  * 等价于 rpc_bt_disconnect_ex(false)，保留旧接口语义。
  *
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_disconnect(void);

  /**
  * @brief 断开当前蓝牙连接，可选择是否保留配对信息。
  *
  * @param keep_bond true  = 只断开连接，保留配对信息（link key / 设备记录），
  *                          下次可直接回连；
  *                  false = 断开并解绑，与旧固件行为一致。
  *
  * @note 该字段是后加的。老从机固件不解析 keep_bond，无论传什么都按解绑处理，
  *       主机侧收不到区分错误码，需要时请先用 rpc_firmware_version_get() 判断版本。
  *
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_disconnect_ex(bool keep_bond);

  /**
  * @brief 设置蓝牙工作模式。
  *
  * @param mode 目标工作模式 (BT_WORK_MODE_EMITTER 或 BT_WORK_MODE_HFP)。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_work_mode_set(bt_work_mode_t mode);

  /**
  * @brief 获取当前蓝牙工作模式。
  *
  * @param mode_out 输出工作模式，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_work_mode_get(bt_work_mode_t *mode_out);

  /**
  * @brief 设置从端蓝牙名称。
  *
  * @param name 目标蓝牙名称，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_name_set(const char *name);

  /**
  * @brief 获取从端蓝牙名称。
  *
  * @param name_out 输出缓冲区，需由调用方保证非空。
  * @param name_out_size 输出缓冲区大小，必须大于 0。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_name_get(char *name_out, size_t name_out_size);

  /**
  * @brief 获取当前蓝牙连接详细状态。
  *
  * @param info_out 输出蓝牙连接信息，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_conn_info_get(bt_connection_info_t *info_out);

  /**
  * @brief 控制 SCO 连接建立或断开。
  *
  * @param connect true 表示建立 SCO/通话，false 表示断开 SCO/通话。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_sco_ctrl(bool connect);

  /**
  * @brief 设置闹钟。
  *
  * 同一个 index 重复调用就是覆盖，没有单独的「修改」接口；
  * 启用/停用也走这里 —— 把原参数原样带上、只改 req->sw 即可（见 alarm_sw_t）。
  *
  * @param req 闹钟请求参数，需由调用方保证非空。
  *            注意 sw 为 0 是「未指定」，从端按启用处理，停用必须写 ALARM_SW_OFF。
  * @param index_out 输出实际分配到的闹钟索引，可为 NULL。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_alarm_set(const alarm_set_req_t *req, uint32_t *index_out);

  /**
  * @brief 取消闹钟。
  *
  * @param index 闹钟索引；当 all 为 true 时该值会被忽略。
  * @param all 是否清空全部闹钟。
  * @param canceled_index_out 输出实际取消的闹钟索引，可为 NULL。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_alarm_cancel(uint32_t index, bool all, uint32_t *canceled_index_out);

  /**
  * @brief 查询闹钟。
  *
  * @param index 闹钟索引。
  * @param resp_out 输出闹钟状态，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_alarm_get(uint32_t index, alarm_get_resp_t *resp_out);

  /**
  * @brief 配置或注销一个从端 GPIO 按键。
  *
  * 按键事件会通过 RPC_VB_EVENT 事件基和 VB_EVT_BUTTON 事件上报。
  *
  * @param conf 按键配置，需由调用方保证非空；conf->enable 为 false 时注销该 id 的按键。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_button_config(const button_config_req_t *conf);

  /**
  * @brief 配置一个从端 GPIO。
  *
  * @param conf GPIO 配置，需由调用方保证非空。
  * @param level_out 可选输出配置后读取到的电平，可为 NULL。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_gpio_config(const gpio_config_req_t *conf, bool *level_out);

  /**
  * @brief 读取一个从端 GPIO 电平。
  *
  * @param io JL IO 编号，使用 rpc_gpio.h 中的 RPC_PORT* 宏。
  * @param level_out 输出电平，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_gpio_read(uint32_t io, bool *level_out);

  /**
  * @brief 写入一个从端 GPIO 输出电平。
  *
  * @param io JL IO 编号，使用 rpc_gpio.h 中的 RPC_PORT* 宏。
  * @param level 目标输出电平。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_gpio_write(uint32_t io, bool level);

  /**
  * @brief 获取从端电池电量信息。
  *
  * @param info_out 输出电量信息，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_battery_get(battery_get_resp_t *info_out);

  /**
  * @brief 开关从端蓝牙。
  *
  * @param on true 表示开启蓝牙，false 表示关闭蓝牙。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_bt_switch(bool on);

  /**
  * @brief 获取从端固件版本信息，以及本机蓝牙 MAC。
  *
  * MAC 只在 ver_out->mac_len == 6 时有效，为 0 表示对端固件旧、没有这个字段，
  * 或者查询时它的蓝牙还没起来（后者重试可得）。不要拿全 0 当"没有"。
  *
  * @param ver_out 输出固件版本信息，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_firmware_version_get(firmware_version_resp_t *ver_out);

  /**
  * @brief 获取当前播放曲目的索引。
  *
  * @param index_out 输出曲目索引 (0-based)，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_music_playing_index_get(uint32_t *index_out);

  /**
  * @brief 控制 mic 数据是否上传给主机（仅控制发送，本地采集/AEC/编码不变）。
  *
  * @param enable true 表示上传 mic 数据（默认），false 表示停止上传。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_mic_data_ctrl(bool enable);


  /**
  * @brief 启动 OTA 会话并获取从端接受的分片参数。
  *
  * @param req OTA begin 请求参数，需由调用方保证非空。
  * @param resp_out 输出 begin 响应，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_ota_begin(const ota_begin_req_t *req, ota_begin_resp_t *resp_out);

  /**
  * @brief 发送一片 OTA 数据。
  *
  * @param req OTA chunk 请求参数，需由调用方保证非空。
  * @param resp_out 输出 chunk 响应，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_ota_chunk(const ota_chunk_req_t *req, ota_chunk_resp_t *resp_out);

  /**
  * @brief 提交 OTA 会话并等待最终校验结果。
  *
  * @param req OTA commit 请求参数，需由调用方保证非空。
  * @param resp_out 输出 commit 响应，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_ota_commit(const ota_commit_req_t *req, ota_commit_resp_t *resp_out);

  /**
  * @brief 中止当前 OTA 会话。
  *
  * @param req OTA abort 请求参数，需由调用方保证非空。
  * @param resp_out 输出 abort 响应，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_ota_abort(const ota_abort_req_t *req, ota_abort_resp_t *resp_out);

  /**
  * @brief 查询当前 OTA 会话状态。
  *
  * @param req OTA status query 请求参数，需由调用方保证非空。
  * @param resp_out 输出状态响应，需由调用方保证非空。
  * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
  */
int rpc_ota_status_query(const ota_status_query_req_t *req, ota_status_query_resp_t *resp_out);

#ifdef __cplusplus
}
#endif

#endif