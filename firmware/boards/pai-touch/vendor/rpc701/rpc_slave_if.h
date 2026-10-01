#ifndef RPC_SLAVE_IF_H
#define RPC_SLAVE_IF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RPC Error Codes */
#define RPC_ERR_SUCCESS                      0
#define RPC_ERR_FAILURE                      -1
#define RPC_ERR_TIMEOUT                      -10
#define RPC_ERR_NO_MEMORY                    -11
#define RPC_ERR_INVALID_PARAM                -12
#define RPC_ERR_TABLE_FULL                   -13
#define RPC_ERR_UID_NOT_FOUND                -14
#define RPC_ERR_ENCODE_FAILED                -15
#define RPC_ERR_DECODE_FAILED                -16
#define RPC_ERR_TX_FAILED                    -17
#define RPC_ERR_BUSY                         -18
#define RPC_ERR_UNSUPPORTED_MSG              -19

/* Default timeout for RPC response (seconds) */
#define DEFAULT_RPC_RSP_TIMEOUT_SEC          5

typedef enum{
    CODEC_OPUS_20MS = 0,
    CODEC_OPUS_40MS = 1,
    CODEC_OPUS_60MS = 2,
    CODEC_PCM_8K = 3,
    CODEC_PCM_16K = 4
}codec_fmt_t;

typedef struct {
    int keep_alive_ms;
    int pa_io;
    int pa_en_level;
    codec_fmt_t coder_fmt;
    codec_fmt_t decoder_fmt;
    char bt_name[32];
    int debug_uart_io;
}vb_init_conf_t;

typedef struct {
    int32_t type;
    char word_name[64];
}wake_word_info_t;

typedef struct {
    wake_word_info_t *words_list;
    size_t n_words;
} wake_word_list_t;

typedef enum {
    MUSIC_MODE_BT = 0,
    MUSIC_MODE_SD = 1,
    MUSIC_MODE_USB = 2,
    MUSIC_MODE_IN_FLASH = 3,
    MUSIC_MODE_MAX = 4,
} music_mode_t;

typedef enum {
    MUSIC_LOOP_MODE_ORDER = 0,
    MUSIC_LOOP_MODE_SINGLE = 1,
    MUSIC_LOOP_MODE_RANDOM = 2,
    MUSIC_LOOP_MODE_MAX = 3,
} music_loop_mode_t;

typedef enum {
    MUSIC_PLAY_CTRL_PLAY = 0,
    MUSIC_PLAY_CTRL_PAUSE = 1,
    MUSIC_PLAY_CTRL_TOGGLE = 2,
    MUSIC_PLAY_CTRL_MAX = 3,
} music_play_ctrl_t;


typedef struct
{
    char *title;
    size_t title_len;
}event_music_title_t;

typedef struct
{
    char *lyrc;
    size_t lyrc_len;
}event_music_lyrc_t;

typedef struct
{
    char word[32];
    size_t word_len;
}event_asr_word_t;


typedef struct
{
    int32_t current_time_sec;
    int32_t total_time_sec;
}event_music_time_t;

typedef struct
{
    int32_t volume;
}event_volume_changed_t;

typedef struct
{
    music_mode_t mode;
}event_music_mode_changed_t;

typedef struct
{
    char *name;
    size_t name_len;
    char *addr;
    size_t addr_len;
    uint32_t dev_class;
    int32_t rssi;
} event_emitter_scan_result_t;

typedef enum {
    STORAGE_DEV_SD0 = 0,
    STORAGE_DEV_SD1 = 1,
    STORAGE_DEV_USB = 2
} storage_dev_type_t;

typedef enum {
    STORAGE_DEV_ACTION_INSERT = 0,
    STORAGE_DEV_ACTION_REMOVE = 1
} storage_dev_action_t;

typedef struct {
    storage_dev_type_t dev;
    storage_dev_action_t action;
} event_storage_dev_changed_t;

typedef struct {
    uint8_t addr[6];
    bool is_sink;
    char name[32];
    size_t name_len;
} event_bt_connected_t;

typedef struct {
    uint8_t addr[6];
} event_bt_disconnected_t;

typedef struct {
    bool connected;
} event_hfp_status_t;

typedef struct {
    bool connected;
} event_sco_status_t;

typedef struct {
    int32_t angle;
} event_doa_angle_t;

typedef struct {
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t index;
} event_alarm_fired_t;

typedef struct {
    uint8_t addr[6];  // BD address 6 bytes
} emitter_connect_conf_t;

typedef enum {
    ASR_MIC_MODE_AEC = 0,
    ASR_MIC_MODE_ANS = 1
} asr_mic_mode_t;

typedef struct {
    asr_mic_mode_t mode;
    uint32_t gain;
} asr_mic_mode_conf_t;

// 单个音乐文件信息
typedef struct {
    char file_name[64];  // 文件名 (UTF-8)
    uint32_t index;       // 文件索引 (0-based)
} sd_music_file_info_t;

// 获取SD卡音乐数量 - 请求
typedef struct {
    storage_dev_type_t dev;  // 设备类型 (SD0/SD1/USB)
} sd_music_count_req_t;

// 获取SD卡音乐数量 - 响应
typedef struct {
    uint32_t total;          // 音乐文件总数
} sd_music_count_resp_t;

// 获取SD卡音乐列表 - 请求
typedef struct {
    storage_dev_type_t dev;  // 设备类型 (SD0/SD1/USB)
    uint32_t offset;         // 起始索引 (0-based)
    uint32_t count;          // 请求的数量
} sd_music_list_req_t;

// 获取SD卡音乐列表 - 响应
typedef struct {
    int32_t resp;            // 错误码 (0=成功)
    uint32_t offset;         // 本次返回的起始索引
    uint32_t count;          // 本次返回的数量
    uint32_t total;          // 音乐文件总数
    sd_music_file_info_t *files;  // 文件列表
    size_t n_files;          // 文件数量
} sd_music_list_resp_t;

typedef struct {
    music_mode_t mode;
} music_mode_set_req_t;

typedef struct {
    music_mode_t mode;
} music_mode_get_resp_t;

typedef struct {
    bool next;
} music_track_switch_req_t;

typedef struct {
    music_play_ctrl_t ctrl;
} music_play_ctrl_req_t;

typedef struct {
    bool is_playing;
} music_is_playing_resp_t;

typedef struct {
    music_loop_mode_t loop_mode;
} music_loop_mode_set_req_t;

typedef struct {
    music_loop_mode_t loop_mode;
} music_loop_mode_get_resp_t;

typedef struct {
    int32_t volume;
} music_volume_set_req_t;

typedef struct {
    int32_t volume;
} music_volume_get_resp_t;

typedef struct {
    uint32_t index;
} music_play_by_index_req_t;

typedef struct {
    bool inserted;
} sd_card_status_resp_t;

typedef struct {
    bool connected;
} bt_status_resp_t;

typedef struct {
    int32_t keep_alive_ms;
} heartbeat_set_req_t;

typedef struct {
    int32_t keep_alive_ms;
} heartbeat_set_resp_t;

typedef struct {
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} time_get_resp_t;

typedef struct {
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} time_set_req_t;

typedef struct {
    /* true  = 只断开当前连接, 保留配对信息, 下次可直接回连
     * false = 断开并解绑(删 link key + 删设备记录), 与该字段出现之前的行为一致 */
    bool keep_bond;
} bt_disconnect_req_t;

typedef enum {
    BT_WORK_MODE_EMITTER = 0,
    BT_WORK_MODE_HFP = 1,
} bt_work_mode_t;

typedef struct {
    bt_work_mode_t mode;
} bt_work_mode_set_req_t;

typedef struct {
    bt_work_mode_t mode;
} bt_work_mode_get_resp_t;

typedef struct {
    char name[32];
} bt_name_set_req_t;

typedef struct {
    char name[32];
    size_t name_len;
} bt_name_get_resp_t;

typedef enum {
    BT_A2DP_STATUS_IDLE = 0,
    BT_A2DP_STATUS_STARTING = 1,
    BT_A2DP_STATUS_SUSPENDING = 2,
} bt_a2dp_status_t;

typedef struct {
    bool connected;
    uint8_t addr[6];
    size_t addr_len;
    char name[32];
    size_t name_len;
    uint32_t dev_class;
    bool is_sink;
    bool support_hfp;
    bool has_mic;
    bool support_cvsd;
    bool support_msbc;
    bool sco_connected;
    bt_a2dp_status_t a2dp_status;
    bool a2dp_channel_up;
    bool hfp_slc_up;
} bt_connection_info_t;

typedef struct {
    bool connect;
} sco_ctrl_req_t;

/* 闹钟开关字段 Rpc_Req_AlarmSet.sw 的取值。
 * 0 是「没填」而不是「关」—— 老主机不带这个字段，从端按启用处理。
 * 所以想停用必须显式写 ALARM_SW_OFF，memset 出来的 0 是启用。 */
typedef enum {
    ALARM_SW_UNSPEC = 0,
    ALARM_SW_ON     = 1,
    ALARM_SW_OFF    = 2,   /* 保留槽位，只停用 */
} alarm_sw_t;

typedef struct {
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t index;
    uint32_t mode;
    uint32_t sw;        /* alarm_sw_t */
} alarm_set_req_t;

typedef struct {
    uint32_t index;
} alarm_set_resp_t;

typedef struct {
    uint32_t index;
    bool all;
} alarm_cancel_req_t;

typedef struct {
    uint32_t index;
    bool all;
} alarm_cancel_resp_t;

typedef struct {
    uint32_t index;
} alarm_get_req_t;

/* 这里没有单独的 sw 字段，但 active 就是启用位（对应 Rpc_Req_AlarmSet.sw：
 * sw==ALARM_SW_OFF 时这里读回 false，其余读回 true）。
 * ⚠ active **不表示「这个槽位有没有配置闹钟」**——空槽位查询直接在
 * resp_event_status 上报错，rpc_alarm_get() 会返回非 RPC_ERR_SUCCESS；
 * active 只在查询成功（槽位确实配置过）时才有意义。 */
typedef struct {
    bool active;
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t index;
    uint32_t mode;
} alarm_get_resp_t;

typedef enum {
    OTA_TRANSPORT_RPC = 0,
    OTA_TRANSPORT_BLE = 1,
} ota_transport_id_t;

typedef enum {
    OTA_POLICY_AUTO = 0,
    OTA_POLICY_RPC_PREFERRED = 1,
    OTA_POLICY_BLE_PREFERRED = 2,
    OTA_POLICY_DUAL_BANK_REQUIRED = 3,
} ota_target_policy_t;

typedef enum {
    OTA_SESSION_IDLE = 0,
    OTA_SESSION_OPENED = 1,
    OTA_SESSION_RECVING = 2,
    OTA_SESSION_VERIFY_PENDING = 3,
    OTA_SESSION_READY = 4,
    OTA_SESSION_FAILED = 5,
    OTA_SESSION_ABORTED = 6,
} ota_session_state_t;

typedef enum {
    OTA_REASON_NONE = 0,
    OTA_REASON_SESSION_BUSY = 1,
    OTA_REASON_BAD_STATE = 2,
    OTA_REASON_BAD_SESSION = 3,
    OTA_REASON_BAD_OFFSET = 4,
    OTA_REASON_BAD_SEQUENCE = 5,
    OTA_REASON_CHUNK_TOO_LARGE = 6,
    OTA_REASON_CRC_MISMATCH = 7,
    OTA_REASON_SIZE_MISMATCH = 8,
    OTA_REASON_CAPACITY_NOT_ENOUGH = 9,
    OTA_REASON_UNSUPPORTED_MODE = 10,
    OTA_REASON_INTERNAL = 11,
} ota_reason_code_t;

typedef struct {
    uint32_t session_id;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t chunk_size;
    uint32_t protocol_version;
    ota_target_policy_t target_policy;
    ota_transport_id_t transport_hint;
    const uint8_t *resume_token;
    size_t resume_token_len;
} ota_begin_req_t;

typedef struct {
    ota_session_state_t state;
    ota_reason_code_t reason_code;
    uint32_t accepted_chunk_size;
    uint32_t next_offset;
    ota_transport_id_t transport_id;
    bool resume_supported;
} ota_begin_resp_t;

typedef struct {
    uint32_t session_id;
    uint32_t seq;
    uint32_t offset;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t payload_crc32;
} ota_chunk_req_t;

typedef struct {
    ota_session_state_t state;
    ota_reason_code_t reason_code;
    uint32_t accepted_seq;
    uint32_t accepted_offset;
    uint32_t next_offset;
} ota_chunk_resp_t;

typedef struct {
    uint32_t session_id;
    uint32_t total_size;
    uint32_t total_chunks;
    uint32_t final_crc32;
} ota_commit_req_t;

typedef struct {
    ota_session_state_t state;
    ota_reason_code_t reason_code;
    uint32_t received_size;
} ota_commit_resp_t;

typedef struct {
    uint32_t session_id;
    ota_reason_code_t reason_code;
} ota_abort_req_t;

typedef struct {
    ota_session_state_t state;
    ota_reason_code_t reason_code;
} ota_abort_resp_t;

typedef struct {
    uint32_t session_id;
} ota_status_query_req_t;

typedef struct {
    ota_session_state_t state;
    ota_reason_code_t reason_code;
    uint32_t received_size;
    uint32_t next_offset;
    uint32_t next_seq;
    uint32_t chunk_size;
    uint32_t image_size;
    uint32_t image_crc32;
} ota_status_query_resp_t;

typedef struct {
    bool on;
} bt_switch_req_t;

typedef struct {
    bool enable;
} mic_data_ctrl_req_t;

typedef enum {
    RPC_BUTTON_EVENT_DOWN = 0,
    RPC_BUTTON_EVENT_CLICK = 1,
    RPC_BUTTON_EVENT_DOUBLE_CLICK = 2,
    RPC_BUTTON_EVENT_REPEAT_CLICK = 3,
    RPC_BUTTON_EVENT_SHORT_START = 4,
    RPC_BUTTON_EVENT_SHORT_UP = 5,
    RPC_BUTTON_EVENT_LONG_START = 6,
    RPC_BUTTON_EVENT_LONG_UP = 7,
    RPC_BUTTON_EVENT_LONG_HOLD = 8,
    RPC_BUTTON_EVENT_LONG_HOLD_UP = 9,
} rpc_button_event_t;

typedef struct {
    uint32_t id;                    /* 按键 id, 0..255 */
    uint32_t io;                    /* JL IO 编号，使用 rpc_gpio.h 中的 RPC_PORT* 宏 */
    bool pressed_logic_level;       /* true=高电平按下 */
    uint32_t short_press_ms;
    uint32_t long_press_ms;
    uint32_t long_hold_ms;
    bool enable;                    /* false 时注销该 id 的按键 */
} button_config_req_t;

typedef enum {
    RPC_GPIO_DIRECTION_INPUT = 0,
    RPC_GPIO_DIRECTION_OUTPUT = 1,
} rpc_gpio_direction_t;

typedef struct {
    uint32_t io;                    /* JL IO 编号，使用 rpc_gpio.h 中的 RPC_PORT* 宏 */
    rpc_gpio_direction_t direction;
    bool pull_up;
    bool pull_down;
    bool die;                       /* 数字输入使能 */
} gpio_config_req_t;

typedef struct {
    uint32_t io;
} gpio_read_req_t;

typedef struct {
    uint32_t io;
    bool level;
} gpio_write_req_t;

/* GpioConfig/GpioRead/GpioWrite 共用响应 */
typedef struct {
    bool level;
} gpio_level_resp_t;

typedef struct {
    int32_t percent;        /* 电量百分比 */
    int32_t voltage_mv;     /* 电池电压(滤波后), mV */
    bool charging;
    int32_t voltage_rt_mv;  /* 实时电压, mV */
} battery_get_resp_t;

/* 蓝牙 MAC 的字节数。协议里是变长 bytes，但对端只会给 6 字节或不给。 */
#define RPC_BT_MAC_LEN                       6

typedef struct {
    char name[32];
    size_t name_len;
    char version[32];
    size_t version_len;
    char build_date[16];
    size_t build_date_len;
    char build_time[16];
    size_t build_time_len;
    /* 本机蓝牙 MAC，6 字节**原始顺序**（与对端的 bt_get_mac_addr 一致，不倒序）。
     * FST 加密语法包就是按这个顺序绑定设备的，可以直接拿来拼下载地址。
     *
     * ⚠ 判"有没有"一律看 mac_len，不要拿全 0 当没有 —— 00:00:00:00:00:00
     * 也是个合法 MAC。mac_len == 0 有两种原因：对端固件旧、没这个字段；
     * 或者查询时它的蓝牙还没起来 —— 后者过一会儿再查就有了，值得重试一次。
     * mac_len 非 0 时 mac 之外的字节被填 0，短给也不会读到脏数据。 */
    uint8_t mac[RPC_BT_MAC_LEN];
    size_t mac_len;
} firmware_version_resp_t;

typedef struct {
    uint32_t index;
} music_playing_index_resp_t;

typedef struct {
    uint32_t id;
    rpc_button_event_t event;
    uint32_t click_cnt;
    uint32_t scan_cnt;
} event_button_t;

typedef struct {
    char word[32];
    size_t word_len;
    uint32_t seq;   /* 同一次识别重发多帧 seq 相同，应用层按 seq 去重 */
} event_asr_word_ex_t;

#ifdef __cplusplus
}
#endif

#endif /* RPC_SLAVE_IF_H */
