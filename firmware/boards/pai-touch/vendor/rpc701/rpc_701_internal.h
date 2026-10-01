#ifndef RPC_701_INTERNAL_H
#define RPC_701_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "rpc_701.h"
#include "rpc_gpio.h"
#include "rpc_slave_if.h"
#include "rpc_transfer.h"
#include "rpc_wrap.h"

#include "port_esp_os.h"
#include "protobuf/rpc_messages.pb-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- internal macros (formerly in rpc_701.h) ---------------- */

#define H_MIN(a, b) (((a) < (b)) ? (a) : (b))

#define H_FREE_PTR_WITH_FUNC(FreeFunc, FreePtr) do {	\
	if (FreeFunc && FreePtr) {             \
		FreeFunc(FreePtr);                 \
		FreePtr = NULL;                    \
	}                                      \
} while (0);

#define RPC_FREE_BUFFS() {                                                    \
  uint8_t idx = 0;                                                            \
  for (idx=0;idx<app_req->n_rpc_free_buff_hdls; idx++)                        \
    HOSTED_FREE(app_req->rpc_free_buff_hdls[idx]);                            \
}

#define CLEANUP_APP_MSG(app_msg) do {                                         \
  if (app_msg) {                                                              \
    if (app_msg->app_free_buff_hdl) {                                         \
      if (app_msg->app_free_buff_func) {                                      \
        app_msg->app_free_buff_func(app_msg->app_free_buff_hdl);              \
        app_msg->app_free_buff_hdl = NULL;                                    \
      }                                                                       \
    }                                                                         \
    HOSTED_FREE(app_msg);                                                     \
  }                                                                           \
} while(0);

#define RPC_FAIL_ON_NULL_PRINT(msGparaM, prinTmsG)                            \
    if (!msGparaM) {                                                          \
        ESP_LOGE(TAG, prinTmsG"\n");                                          \
        goto fail_parse_rpc_msg;                                              \
    }

#define RPC_FAIL_ON_NULL(msGparaM)                                            \
    if (!rpc_msg->msGparaM) {                                                 \
        ESP_LOGE(TAG, "Failed to process rx data\n");                         \
        goto fail_parse_rpc_msg;                                              \
    }

#define NTFY_ALLOC_ELEMENT(TyPe,MsG_StRuCt,InIt_FuN) {                        \
  TyPe *NeW_AllocN = (TyPe *)calloc(1, sizeof(TyPe));                         \
  if (!NeW_AllocN) {                                                          \
    ESP_LOGI(TAG,"Failed to allocate memory for req.%s\n",#MsG_StRuCt);       \
    ntfy_payload->resp = RPC_ERR_MEMORY_FAILURE;                              \
    goto err;                                                                 \
  }                                                                           \
  MsG_StRuCt = NeW_AllocN;                                                    \
  InIt_FuN(MsG_StRuCt);                                                       \
}

#define NTFY_COPY_BYTES(dest, src, num)                                         \
  do {                                                                          \
    if (num) {                                                                  \
      dest.data = (uint8_t *)calloc(1, num);                                    \
      if (!dest.data) {                                                         \
        XZ_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);   \
        ntfy_payload->resp = FAILURE;                                           \
        return ESP_OK;                                                          \
      }                                                                         \
      memcpy(dest.data, src, num);                                              \
      dest.len = num;                                                           \
    }                                                                           \
  } while(0)

#define NTFY_COPY_STR(dest, src, max_len)                                       \
  if (src) {                                                                    \
    dest.data = (uint8_t*)strndup((char*)src, max_len);                         \
    if (!dest.data) {                                                           \
      ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);     \
      ntfy_payload->resp = FAILURE;                                             \
      return ESP_OK;                                                            \
    }                                                                           \
    dest.len = min(max_len,strlen((char*)src)+1);                                 \
  }

#define RPC_REQ_COPY_BYTES(DsT,SrC,SizE) {                                    \
  if (SizE && SrC) {                                                          \
	DsT.data = SrC;                                                           \
	DsT.len = SizE;                                                           \
  }                                                                           \
}

#define RPC_REQ_COPY_STR(DsT,SrC,MaxSizE) {                                   \
  if (SrC) {                                                                  \
    RPC_REQ_COPY_BYTES(DsT, (uint8_t*)SrC, H_MIN(strlen((char*)SrC)+1,MaxSizE));        \
  }                                                                           \
}

/* protobuf bytes → 定长二进制缓冲：截断拷贝并回写实际长度（len_out 可为 NULL）。
 *
 * 与下面的 rpc_copy_pb_str 的区别在于**不补也不预留 NUL**：MAC 这种正好 6 字节
 * 的字段按字符串搬会被砍成 5 字节。没给或短给时余下的字节填 0，调用方就算忘了
 * 看 len_out 也不会读到上一次请求留下的脏数据。 */
static inline void rpc_copy_pb_bytes(uint8_t *dst, size_t cap, size_t *len_out,
                                     const ProtobufCBinaryData *src)
{
    size_t copy_len = src->data ? src->len : 0;
    if (copy_len > cap) {
        copy_len = cap;
    }
    if (copy_len) {
        memcpy(dst, src->data, copy_len);
    }
    memset(dst + copy_len, 0, cap - copy_len);
    if (len_out) {
        *len_out = copy_len;
    }
}

/* protobuf bytes → 定长 char 缓冲：截断拷贝、NUL 结尾并回写实际长度（len_out 可为 NULL） */
static inline void rpc_copy_pb_str(char *dst, size_t cap, size_t *len_out,
                                   const ProtobufCBinaryData *src)
{
    size_t copy_len = src->data ? src->len : 0;
    if (copy_len > cap - 1) {
        copy_len = cap - 1;
    }
    if (copy_len) {
        memcpy(dst, src->data, copy_len);
    }
    dst[copy_len] = '\0';
    if (len_out) {
        *len_out = copy_len;
    }
}

typedef struct q_element {
    void *buf;
    int buf_len;
} esp_queue_elem_t;

/* ---------------- internal callback macros (formerly in rpc_slave_if.h) ---------------- */

#define CALLBACK_SET_SUCCESS                 0
#define CALLBACK_AVAILABLE                   0
#define CALLBACK_NOT_REGISTERED              -1
#define MSG_ID_OUT_OF_ORDER                  -2

#define MAX_FREE_BUFF_HANDLES          20

/* ---------------- ctrl_cmd_t (formerly in rpc_slave_if.h) ---------------- */

typedef struct ctrl_cmd {
        /* 消息类型: 1=req, 2=resp, 3=event */
    uint8_t msg_type;

    /* 控制 path protobuf 消息编号 */
    uint16_t msg_id;

    /* 请求/响应的 UID */
    uint32_t uid;

    /* 响应或事件状态 */
    int32_t resp_event_status;

    /* 同步模式型号量*/
    void *rx_sem;
    union
    {
        vb_init_conf_t vb_init_conf;
        emitter_connect_conf_t emitter_connect_conf;
        wake_word_list_t get_wake_word_list_resp;
        asr_mic_mode_conf_t asr_mic_mode_conf;
        sd_music_count_req_t sd_music_count_req;
        sd_music_count_resp_t sd_music_count_resp;
        sd_music_list_req_t sd_music_list_req;
        sd_music_list_resp_t sd_music_list_resp;
        music_mode_set_req_t music_mode_set_req;
        music_mode_get_resp_t music_mode_get_resp;
        music_track_switch_req_t music_track_switch_req;
        music_play_ctrl_req_t music_play_ctrl_req;
        music_is_playing_resp_t music_is_playing_resp;
        music_loop_mode_set_req_t music_loop_mode_set_req;
        music_loop_mode_get_resp_t music_loop_mode_get_resp;
        music_volume_set_req_t music_volume_set_req;
        music_volume_get_resp_t music_volume_get_resp;
        music_play_by_index_req_t music_play_by_index_req;
        sd_card_status_resp_t sd_card_status_resp;
        bt_status_resp_t bt_status_resp;
        heartbeat_set_req_t heartbeat_set_req;
        heartbeat_set_resp_t heartbeat_set_resp;
        time_get_resp_t time_get_resp;
        time_set_req_t time_set_req;
        bt_disconnect_req_t bt_disconnect_req;
        bt_work_mode_set_req_t bt_work_mode_set_req;
        bt_work_mode_get_resp_t bt_work_mode_get_resp;
        bt_name_set_req_t bt_name_set_req;
        bt_name_get_resp_t bt_name_get_resp;
        bt_connection_info_t bt_conn_info_get_resp;
        sco_ctrl_req_t sco_ctrl_req;
        alarm_set_req_t alarm_set_req;
        alarm_set_resp_t alarm_set_resp;
        alarm_cancel_req_t alarm_cancel_req;
        alarm_cancel_resp_t alarm_cancel_resp;
        alarm_get_req_t alarm_get_req;
        alarm_get_resp_t alarm_get_resp;
        ota_begin_req_t ota_begin_req;
        ota_begin_resp_t ota_begin_resp;
        ota_chunk_req_t ota_chunk_req;
        ota_chunk_resp_t ota_chunk_resp;
        ota_commit_req_t ota_commit_req;
        ota_commit_resp_t ota_commit_resp;
        ota_abort_req_t ota_abort_req;
        ota_abort_resp_t ota_abort_resp;
        ota_status_query_req_t ota_status_query_req;
        ota_status_query_resp_t ota_status_query_resp;
        button_config_req_t button_config_req;
        gpio_config_req_t gpio_config_req;
        gpio_read_req_t gpio_read_req;
        gpio_write_req_t gpio_write_req;
        gpio_level_resp_t gpio_level_resp;
        battery_get_resp_t battery_get_resp;
        bt_switch_req_t bt_switch_req;
        firmware_version_resp_t firmware_version_resp;
        music_playing_index_resp_t music_playing_index_resp;
        mic_data_ctrl_req_t mic_data_ctrl_req;

        event_music_title_t music_info_title_evt;
        event_music_lyrc_t music_info_lyrc_evt;
        event_music_time_t music_info_time_evt;
        event_volume_changed_t volume_changed_evt;
        event_music_mode_changed_t music_mode_changed_evt;
        event_asr_word_t asr_word;
        event_emitter_scan_result_t emitter_scan_result_evt;
        event_storage_dev_changed_t storage_dev_changed_evt;
        event_bt_connected_t bt_connected_evt;
        event_bt_disconnected_t bt_disconnected_evt;
        event_hfp_status_t hfp_status_evt;
        event_sco_status_t sco_status_evt;
        event_doa_angle_t doa_angle_evt;
        event_alarm_fired_t alarm_fired_evt;
        event_button_t button_evt;
        event_asr_word_ex_t asr_word_ex_evt;
    }u;

        /**
     * 响应回调函数
     * - 设置此回调 -> 异步模式
     * - NULL -> 同步模式
     *
     * 异步契约：
     * - cb 在 RPC RX 任务上下文执行（超时/发送失败时在 esp_timer 任务），
     *   不得阻塞、不得再同步发 RPC，拷走数据后立即返回。
     * - resp 仅在 cb 调用期间有效，cb 返回后由框架释放（含
     *   app_free_buff_hdl 挂的附属缓冲），cb 不得自行 free。
     * - 超时/失败时 resp->resp_event_status 为对应错误码，payload 无效。
     */
    int (*rpc_rsp_cb)(struct ctrl_cmd *data);

    /* 异步模式的用户上下文：框架把 req 的 user_ctx 拷到 resp 后再调 cb */
    void *user_ctx;

    /* 响应超时时间（秒），默认为 DEFAULT_RPC_RSP_TIMEOUT */
    int rsp_timeout_sec;

    /* 等待前一个命令完成的时间（秒） */
    int wait_prev_cmd_completion;

    /* 需要下层释放的数据指针（如果为 NULL 则忽略） */
    void *app_free_buff_hdl;

    /* 释放句柄的函数（如果为 NULL 则忽略） */
    void (*app_free_buff_func)(void *app_free_buff_hdl);

    /* RPC 内部需要释放的缓冲区句柄 */
    void *rpc_free_buff_hdls[MAX_FREE_BUFF_HANDLES];
    uint8_t n_rpc_free_buff_hdls;
}ctrl_cmd_t;

/* resp callback */
typedef int (*rpc_rsp_cb_t) (ctrl_cmd_t * resp);

/* event callback */
typedef int (*rpc_evt_cb_t) (ctrl_cmd_t * event);

#include "rpc_ota.h"

/* ---------------- internal slave-if dispatcher functions ---------------- */

ctrl_cmd_t *rpc_slaveif_vb_init(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_emitter_connect(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_get_wake_word_list(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_emitter_start_scan(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_asr_mic_mode_change(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_sd_music_count(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_sd_music_list(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_mode_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_mode_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_track_switch(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_play_ctrl(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_is_playing(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_loop_mode_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_loop_mode_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_volume_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_volume_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_play_music_by_index(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_power_off(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_heartbeat(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_heartbeat_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_reboot(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_sd_card_status(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_status(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_time_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_time_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_disconnect(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_work_mode_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_work_mode_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_name_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_name_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_conn_info_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_sco_ctrl(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_alarm_set(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_alarm_cancel(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_alarm_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_ota_begin(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_ota_chunk(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_ota_commit(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_ota_abort(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_ota_status_query(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_button_config(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_gpio_config(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_gpio_read(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_gpio_write(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_battery_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_bt_switch(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_firmware_version_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_music_playing_index_get(ctrl_cmd_t *req);
ctrl_cmd_t *rpc_slaveif_mic_data_ctrl(ctrl_cmd_t *req);

/* ---------------- internal transfer (formerly in rpc_transfer.h) ---------------- */

#define SERIAL_PROTO_HEADER       0xAA55
#define SERIAL_PROTO_HEADER_H     0xAA
#define SERIAL_PROTO_HEADER_L     0x55
#define SERIAL_PROTO_MAX_DATA_LEN 1024
#define SERIAL_PROTO_MAX_PACK_LEN (SERIAL_PROTO_MAX_DATA_LEN + 6)

typedef enum {
    PACK_TYPE_RPC = 0,
    PACK_TYPE_AUDIO = 1,
    PACK_TYPE_AUDIO_OPUS = 2,
    PACK_TYPE_MAX,
} serial_pack_type_t;

typedef struct __attribute__((packed)) {
    uint16_t header;
    uint8_t  type;
    uint16_t length;
    uint8_t  data[];
} serial_proto_frame_t;

typedef void (*transfer_recv_cb_t)(uint8_t type, const uint8_t *data, size_t len, void *user_ctx);

void serial_transfer_init(transfer_write_cb_t write_cb, transfer_recv_cb_t recv_cb, void *user_ctx);
void serial_transfer_deinit(void);
void serial_transfer_send(const void *data, int len);
int  serial_transfer_send_packed(serial_pack_type_t type, const void *data, int len);
void serial_transfer_process(uint8_t *data, size_t len);
void serial_transfer_reset(void);

/* ---------------- internal rpc dispatch (formerly in rpc_701.h) ---------------- */

int rpc_send_req(ctrl_cmd_t *app_req);
ctrl_cmd_t *rpc_wait_and_parse_sync_resp(ctrl_cmd_t *app_req);
int compose_rpc_req(Rpc *req, ctrl_cmd_t *app_req, int32_t *failure_status);
int rpc_parse_rsp(Rpc *rpc_msg, ctrl_cmd_t *app_resp);
int rpc_parse_evt(Rpc *rpc_msg, ctrl_cmd_t *app_ntfy);

int set_event_callback(int event, rpc_rsp_cb_t event_cb);
int reset_event_callback(int event);

#ifdef __cplusplus
}
#endif

#endif /* RPC_701_INTERNAL_H */
