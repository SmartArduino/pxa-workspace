#include "stdio.h"
#include "stdint.h"
#include "stdbool.h"
#include <string.h>
#include "rpc_701_internal.h"
#include "rpc_music_now.h"
#include "esp_log.h"

#include <stdlib.h>

static const char* TAG = "rpc_wrap";

typedef struct {
	int event;
	rpc_rsp_cb_t fun;
} event_callback_table_t;

static int post_music_title_event(const event_music_title_t *event)
{
    size_t title_len = 0;
    size_t total_size = 0;
    rpc_vb_music_title_event_t *payload = NULL;
    int result;

    if (event && event->title) {
        title_len = event->title_len;
    }
    total_size = sizeof(rpc_vb_music_title_event_t) + title_len + 1;
    payload = (rpc_vb_music_title_event_t *)calloc(1, total_size);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate music title event payload");
        return RPC_ERR_NO_MEMORY;
    }

    payload->title_len = title_len;
    if (title_len > 0) {
        memcpy(payload->title, event->title, title_len);
    }

    result = rpcp_event_post(RPC_VB_EVENT, VB_EVT_MUSIC_INFO_TITLE,
                             payload, total_size, portMAX_DELAY);
    free(payload);
    return result;
}

static int post_music_lyrc_event(const event_music_lyrc_t *event)
{
    size_t lyrc_len = 0;
    size_t total_size = 0;
    rpc_vb_music_lyrc_event_t *payload = NULL;
    int result;

    if (event && event->lyrc) {
        lyrc_len = event->lyrc_len;
    }
    total_size = sizeof(rpc_vb_music_lyrc_event_t) + lyrc_len + 1;
    payload = (rpc_vb_music_lyrc_event_t *)calloc(1, total_size);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate music lyric event payload");
        return RPC_ERR_NO_MEMORY;
    }

    payload->lyrc_len = lyrc_len;
    if (lyrc_len > 0) {
        memcpy(payload->lyrc, event->lyrc, lyrc_len);
    }

    result = rpcp_event_post(RPC_VB_EVENT, VB_EVT_MUSIC_INFO_LYRC,
                             payload, total_size, portMAX_DELAY);
    free(payload);
    return result;
}

static int post_emitter_scan_result_event(const event_emitter_scan_result_t *event)
{
    size_t name_len = 0;
    size_t total_size = 0;
    rpc_vb_emitter_scan_result_event_t *payload = NULL;
    int result;

    if (event && event->name) {
        name_len = event->name_len;
    }

    total_size = sizeof(rpc_vb_emitter_scan_result_event_t) + name_len + 1;
    payload = (rpc_vb_emitter_scan_result_event_t *)calloc(1, total_size);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate emitter scan result event payload");
        return RPC_ERR_NO_MEMORY;
    }

    payload->name_len = name_len;
    payload->dev_class = event ? event->dev_class : 0;
    payload->rssi = event ? event->rssi : 0;

    if (event && event->addr) {
        memcpy(payload->addr, event->addr, 6);
    }
    if (name_len > 0) {
        memcpy(payload->name, event->name, name_len);
    }

    result = rpcp_event_post(RPC_VB_EVENT, VB_EVT_EMITTER_SCAN_RESULT,
                             payload, total_size, portMAX_DELAY);
    free(payload);
    return result;
}

static ctrl_cmd_t * RPC_DEFAULT_REQ(void)
{
  ctrl_cmd_t *new_req = (ctrl_cmd_t*)calloc(1, sizeof(ctrl_cmd_t));
  assert(new_req);
  new_req->msg_type = RPC_TYPE__Req;
  new_req->rpc_rsp_cb = NULL;
  new_req->rsp_timeout_sec = DEFAULT_RPC_RSP_TIMEOUT_SEC;
  /* new_req->wait_prev_cmd_completion = WAIT_TIME_B2B_RPC_REQ; */
  return new_req;
}

#define CLEANUP_RPC(msg) do {                            \
  if (msg) {                                             \
    if (msg->app_free_buff_hdl) {                        \
      if (msg->app_free_buff_func) {                     \
        msg->app_free_buff_func(msg->app_free_buff_hdl); \
        msg->app_free_buff_hdl = NULL;                   \
      }                                                  \
    }                                                    \
    HOSTED_FREE(msg);                                    \
    msg = NULL;                                          \
  }                                                      \
} while(0);

static int process_failed_responses(ctrl_cmd_t *app_msg)
{
	uint8_t request_failed_flag = true;
	int result = app_msg->resp_event_status;

	/* Identify general issue, common for all control requests */
	/* Map results to a matching ESP_ERR_ code */
	switch (app_msg->resp_event_status) {
		case RPC_ERR_BUSY:
			ESP_LOGE(TAG, "Error reported: Command In progress, Please wait");
			break;
		case RPC_ERR_TIMEOUT:
			ESP_LOGE(TAG, "Error reported: Response Timeout");
			break;
		case RPC_ERR_NO_MEMORY:
			ESP_LOGE(TAG, "Error reported: Memory allocation failed");
			break;
		// case RPC_ERR_UNSUPPORTED_MSG:
		// 	ESP_LOGE(TAG, "Error reported: Unsupported control msg");
		// 	break;
		case RPC_ERR_INVALID_PARAM:
			ESP_LOGE(TAG, "Error reported: Invalid or out of range parameter values");
			break;
		// case RPC_ERR_PROTOBUF_ENCODE:
		// 	ESP_LOGE(TAG, "Error reported: Protobuf encode failed");
		// 	break;
		// case RPC_ERR_PROTOBUF_DECODE:
		// 	ESP_LOGE(TAG, "Error reported: Protobuf decode failed");
		// 	break;
		// case RPC_ERR_SET_ASYNC_CB:
		// 	ESP_LOGE(TAG, "Error reported: Failed to set aync callback");
		// 	break;
		// case RPC_ERR_TRANSPORT_SEND:
		// 	ESP_LOGE(TAG, "Error reported: Problem while sending data on serial driver");
		// 	break;
		default:
			request_failed_flag = false;
			break;
	}

	/* if control request failed, no need to proceed for response checking */
	if (request_failed_flag)
		return result;

	/* Identify control request specific issue */
	
    ESP_LOGD(TAG, "Got Hosted Control Response with resp code %d", result);
	
	return result;
}


int rpc_rsp_callback(ctrl_cmd_t * app_resp)
{
	int response = RPC_ERR_FAILURE; // default response

	if (!app_resp || (app_resp->msg_type != RPC_TYPE__Resp)) {
		if (app_resp)
			ESP_LOGE(TAG, "Recvd Msg[0x%x] is not response",app_resp->msg_type);
		goto fail_resp;
	}

	// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
	if ((app_resp->msg_id < RPC_ID__Resp_Base) || (app_resp->msg_id >= RPC_ID__Resp_Max)) {
		ESP_LOGE(TAG, "Response Msg ID[0x%x] is not correct",app_resp->msg_id);
		goto fail_resp;
	}

	if (app_resp->resp_event_status != RPC_ERR_SUCCESS) {
		response = process_failed_responses(app_resp);
		goto fail_resp;
	}

    switch (app_resp->msg_id)
    {
    case RPC_ID__Resp_VbInit:{
        ESP_LOGV(TAG, "VB Init Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_EmitterConnect:{
        ESP_LOGV(TAG, "Emitter Connect Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_GetAsrWords:{
        ESP_LOGV(TAG, "Get Wake Word List Response: %ld, words count: %zu", 
                 app_resp->resp_event_status, app_resp->u.get_wake_word_list_resp.n_words);
        break;
    }
    case RPC_ID__Resp_EmitterStartScan:{
        ESP_LOGV(TAG, "Emitter Start Scan Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_UserAsrMicModeChange:{
        ESP_LOGV(TAG, "ASR Mic Mode Change Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_SdMusicCount:{
        ESP_LOGV(TAG, "SD Music Count Response: %ld, total: %lu",
                 app_resp->resp_event_status,
                 (unsigned long)app_resp->u.sd_music_count_resp.total);
        break;
    }
    case RPC_ID__Resp_SdMusicList:{
        ESP_LOGV(TAG, "SD Music List Response: %ld, offset: %lu, count: %lu, total: %lu, files: %zu",
                 app_resp->resp_event_status,
                 (unsigned long)app_resp->u.sd_music_list_resp.offset,
                 (unsigned long)app_resp->u.sd_music_list_resp.count,
                 (unsigned long)app_resp->u.sd_music_list_resp.total,
                 app_resp->u.sd_music_list_resp.n_files);
        break;
    }
    case RPC_ID__Resp_MusicModeGet:{
        ESP_LOGV(TAG, "Music Mode Get Response: %ld, mode: %d",
                 app_resp->resp_event_status,
                 app_resp->u.music_mode_get_resp.mode);
        break;
    }
    case RPC_ID__Resp_MusicModeSet:{
        ESP_LOGV(TAG, "Music Mode Set Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_MusicTrackSwitch:{
        ESP_LOGV(TAG, "Music Track Switch Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_PlayMusicByIndex:{
        ESP_LOGV(TAG, "Play Music By Index Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_MusicPlayCtrl:{
        ESP_LOGV(TAG, "Music Play Ctrl Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_MusicIsPlaying:{
        ESP_LOGV(TAG, "Music Is Playing Response: %ld, is_playing: %d",
                 app_resp->resp_event_status,
                 app_resp->u.music_is_playing_resp.is_playing);
        break;
    }
    case RPC_ID__Resp_MusicLoopModeSet:{
        ESP_LOGV(TAG, "Music Loop Mode Set Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_MusicLoopModeGet:{
        ESP_LOGV(TAG, "Music Loop Mode Get Response: %ld, loop_mode: %d",
                 app_resp->resp_event_status,
                 app_resp->u.music_loop_mode_get_resp.loop_mode);
        break;
    }
    case RPC_ID__Resp_MusicVolumeSet:{
        ESP_LOGV(TAG, "Music Volume Set Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_MusicVolumeGet:{
        ESP_LOGV(TAG, "Music Volume Get Response: %ld, volume: %ld",
                 app_resp->resp_event_status,
                 (long)app_resp->u.music_volume_get_resp.volume);
        break;
    }
    case RPC_ID__Resp_PowerOff:{
        ESP_LOGV(TAG, "Power Off Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_Heartbeat:{
        ESP_LOGV(TAG, "Heartbeat Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_HeartbeatSet:{
        ESP_LOGV(TAG, "Heartbeat Set Response: %ld, keep_alive_ms: %ld",
                 app_resp->resp_event_status,
                 (long)app_resp->u.heartbeat_set_resp.keep_alive_ms);
        break;
    }
    case RPC_ID__Resp_Reboot:{
        ESP_LOGV(TAG, "Reboot Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_SdCardStatus:{
        ESP_LOGV(TAG, "SD Card Status Response: %ld, inserted: %d",
                 app_resp->resp_event_status,
                 app_resp->u.sd_card_status_resp.inserted);
        break;
    }
    case RPC_ID__Resp_BtStatus:{
        ESP_LOGV(TAG, "BT Status Response: %ld, connected: %d",
                 app_resp->resp_event_status,
                 app_resp->u.bt_status_resp.connected);
        break;
    }
    case RPC_ID__Resp_TimeGet:{
        ESP_LOGV(TAG, "Time Get Response: %ld, %04lu-%02lu-%02lu %02lu:%02lu:%02lu",
                 app_resp->resp_event_status,
                 (unsigned long)app_resp->u.time_get_resp.year,
                 (unsigned long)app_resp->u.time_get_resp.month,
                 (unsigned long)app_resp->u.time_get_resp.day,
                 (unsigned long)app_resp->u.time_get_resp.hour,
                 (unsigned long)app_resp->u.time_get_resp.minute,
                 (unsigned long)app_resp->u.time_get_resp.second);
        break;
    }
    case RPC_ID__Resp_TimeSet:{
        ESP_LOGV(TAG, "Time Set Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_BtDisconnect:{
        ESP_LOGV(TAG, "BT Disconnect Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_BtWorkModeSet:{
        ESP_LOGV(TAG, "BT WorkMode Set Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_BtWorkModeGet:{
        ESP_LOGV(TAG, "BT WorkMode Get Response: %ld, mode: %d",
                 app_resp->resp_event_status,
                 app_resp->u.bt_work_mode_get_resp.mode);
        break;
    }
    case RPC_ID__Resp_BtNameSet:{
        ESP_LOGV(TAG, "BT Name Set Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_BtNameGet:{
        ESP_LOGV(TAG, "BT Name Get Response: %ld, name: %s",
                 app_resp->resp_event_status,
                 app_resp->u.bt_name_get_resp.name);
        break;
    }
    case RPC_ID__Resp_BtConnInfoGet:{
        ESP_LOGV(TAG, "BT Conn Info Get Response: %ld, connected: %d, sco: %d, a2dp: %d",
                 app_resp->resp_event_status,
                 app_resp->u.bt_conn_info_get_resp.connected,
                 app_resp->u.bt_conn_info_get_resp.sco_connected,
                 app_resp->u.bt_conn_info_get_resp.a2dp_status);
        break;
    }
    case RPC_ID__Resp_ScoCtrl:{
        ESP_LOGV(TAG, "SCO Ctrl Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_AlarmSet:{
        ESP_LOGV(TAG, "Alarm Set Response: %ld, index: %lu",
                 app_resp->resp_event_status,
                 (unsigned long)app_resp->u.alarm_set_resp.index);
        break;
    }
    case RPC_ID__Resp_AlarmCancel:{
        ESP_LOGV(TAG, "Alarm Cancel Response: %ld, index: %lu, all: %d",
                 app_resp->resp_event_status,
                 (unsigned long)app_resp->u.alarm_cancel_resp.index,
                 app_resp->u.alarm_cancel_resp.all);
        break;
    }
    case RPC_ID__Resp_AlarmGet:{
        ESP_LOGV(TAG, "Alarm Get Response: %ld, active: %d, index: %lu, mode: %lu",
                 app_resp->resp_event_status,
                 app_resp->u.alarm_get_resp.active,
                 (unsigned long)app_resp->u.alarm_get_resp.index,
                 (unsigned long)app_resp->u.alarm_get_resp.mode);
        break;
    }
    case RPC_ID__Resp_ButtonConfig:{
        ESP_LOGV(TAG, "Button Config Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_GpioConfig:
    case RPC_ID__Resp_GpioRead:
    case RPC_ID__Resp_GpioWrite:{
        ESP_LOGV(TAG, "GPIO Response[0x%x]: %ld, level: %d",
                 app_resp->msg_id,
                 app_resp->resp_event_status,
                 app_resp->u.gpio_level_resp.level);
        break;
    }
    case RPC_ID__Resp_BatteryGet:{
        ESP_LOGV(TAG, "Battery Get Response: %ld, percent: %ld, voltage: %ldmV, charging: %d",
                 app_resp->resp_event_status,
                 (long)app_resp->u.battery_get_resp.percent,
                 (long)app_resp->u.battery_get_resp.voltage_mv,
                 app_resp->u.battery_get_resp.charging);
        break;
    }
    case RPC_ID__Resp_BtSwitch:{
        ESP_LOGV(TAG, "BT Switch Response: %ld", app_resp->resp_event_status);
        break;
    }
    case RPC_ID__Resp_FirmwareVersionGet:{
        const firmware_version_resp_t *fw = &app_resp->u.firmware_version_resp;
        /* mac_len 也打出来：MAC 全 0 既可能是对端没给，也可能它真是这个值，
         * 光看六个字节分不出来，而"为什么拿不到 MAC"正是这里最常被问的问题。 */
        ESP_LOGV(TAG, "Firmware Version Get Response: %ld, name: %s, version: %s, "
                      "mac: %02x:%02x:%02x:%02x:%02x:%02x (len %u)",
                 app_resp->resp_event_status, fw->name, fw->version,
                 fw->mac[0], fw->mac[1], fw->mac[2],
                 fw->mac[3], fw->mac[4], fw->mac[5], (unsigned)fw->mac_len);
        break;
    }
    case RPC_ID__Resp_MusicPlayingIndexGet:{
        ESP_LOGV(TAG, "Music Playing Index Get Response: %ld, index: %lu",
                 app_resp->resp_event_status,
                 (unsigned long)app_resp->u.music_playing_index_resp.index);
        break;
    }
    case RPC_ID__Resp_MicDataCtrl:{
        ESP_LOGV(TAG, "Mic Data Ctrl Response: %ld", app_resp->resp_event_status);
        break;
    }
    default:{
        ESP_LOGE(TAG, "Unsupported message id %d", app_resp->msg_id);
        goto fail_resp;
    }
    }

// finish_resp:
	// extract response from app_resp
	response = app_resp->resp_event_status;
	CLEANUP_RPC(app_resp);
	return response;

fail_resp:
	CLEANUP_RPC(app_resp);
	return response;
}

int rpc_vb_init(vb_init_conf_t *conf){
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.vb_init_conf.keep_alive_ms = conf->keep_alive_ms;
    req->u.vb_init_conf.pa_io = conf->pa_io;
    req->u.vb_init_conf.pa_en_level = conf->pa_en_level;
    req->u.vb_init_conf.coder_fmt = conf->coder_fmt;
    req->u.vb_init_conf.decoder_fmt = conf->decoder_fmt;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    strncpy(req->u.vb_init_conf.bt_name, conf->bt_name, sizeof(req->u.vb_init_conf.bt_name) - 1);
#pragma GCC diagnostic pop
    req->u.vb_init_conf.debug_uart_io = conf->debug_uart_io;
    req->rsp_timeout_sec = 1;
    resp = rpc_slaveif_vb_init(req);
    return rpc_rsp_callback(resp);
}

int rpc_emitter_connect(const uint8_t *addr){
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    if (!addr) {
        ESP_LOGE(TAG, "Invalid addr parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    memcpy(req->u.emitter_connect_conf.addr, addr, 6);

    resp = rpc_slaveif_emitter_connect(req);
    return rpc_rsp_callback(resp);
}

int rpc_get_wake_word_list(wake_word_list_t *list)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = rpc_slaveif_get_wake_word_list(req);
    int result;

    if (!resp) {
        ESP_LOGE(TAG, "Failed to get wake word list response");
        list->n_words = 0;
        list->words_list = NULL;
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;

    if (result == RPC_ERR_SUCCESS && resp->u.get_wake_word_list_resp.n_words > 0) {
        size_t n_words = resp->u.get_wake_word_list_resp.n_words;

        list->n_words = n_words;
        list->words_list = (wake_word_info_t*)calloc(n_words, sizeof(wake_word_info_t));
        if (!list->words_list) {
            ESP_LOGE(TAG, "Failed to allocate memory for wake word list");
            list->n_words = 0;
            list->words_list = NULL;
            rpc_rsp_callback(resp);
            return RPC_ERR_NO_MEMORY;
        }

        for (size_t i = 0; i < n_words; i++) {
            list->words_list[i].type = resp->u.get_wake_word_list_resp.words_list[i].type;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
            strncpy(list->words_list[i].word_name,
                    resp->u.get_wake_word_list_resp.words_list[i].word_name,
                    sizeof(list->words_list[i].word_name) - 1);
#pragma GCC diagnostic pop
            list->words_list[i].word_name[sizeof(list->words_list[i].word_name) - 1] = '\0';
        }
    } else {
        list->n_words = 0;
        list->words_list = NULL;
    }

    
    return rpc_rsp_callback(resp);
}

int rpc_emitter_start_scan(void)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = rpc_slaveif_emitter_start_scan(req);
    return rpc_rsp_callback(resp);
}

int rpc_asr_mic_mode_change(asr_mic_mode_t mode, uint32_t gain)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.asr_mic_mode_conf.mode = mode;
    req->u.asr_mic_mode_conf.gain = gain;

    resp = rpc_slaveif_asr_mic_mode_change(req);
    return rpc_rsp_callback(resp);
}

int rpc_sd_music_count(storage_dev_type_t dev, uint32_t *total_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!total_out) {
        ESP_LOGE(TAG, "Invalid total_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    *total_out = 0;
    req->u.sd_music_count_req.dev = dev;

    resp = rpc_slaveif_sd_music_count(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get music count response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *total_out = resp->u.sd_music_count_resp.total;
    }

    return rpc_rsp_callback(resp);
}

int rpc_sd_music_list(storage_dev_type_t dev, uint32_t offset, uint32_t count, sd_music_list_resp_t *resp_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!resp_out) {
        ESP_LOGE(TAG, "Invalid resp_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    // 清空输出结构
    memset(resp_out, 0, sizeof(sd_music_list_resp_t));

    req->u.sd_music_list_req.dev = dev;
    req->u.sd_music_list_req.offset = offset;
    req->u.sd_music_list_req.count = count;

    resp = rpc_slaveif_sd_music_list(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get music list response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        resp_out->resp = resp->u.sd_music_list_resp.resp;
        resp_out->offset = resp->u.sd_music_list_resp.offset;
        resp_out->count = resp->u.sd_music_list_resp.count;
        resp_out->total = resp->u.sd_music_list_resp.total;
        resp_out->n_files = resp->u.sd_music_list_resp.n_files;

        if (resp->u.sd_music_list_resp.n_files > 0 && resp->u.sd_music_list_resp.files) {
            resp_out->files = (sd_music_file_info_t*)calloc(
                resp->u.sd_music_list_resp.n_files, 
                sizeof(sd_music_file_info_t)
            );
            if (!resp_out->files) {
                ESP_LOGE(TAG, "Failed to allocate memory for music list");
                resp_out->n_files = 0;
                CLEANUP_RPC(resp);
                return RPC_ERR_NO_MEMORY;
            }

            memcpy(resp_out->files, 
                   resp->u.sd_music_list_resp.files,
                   resp->u.sd_music_list_resp.n_files * sizeof(sd_music_file_info_t));
        }
    }

    return rpc_rsp_callback(resp);
}

/* 异步列表请求的适配层：把框架的 ctrl_cmd_t 回调翻译成
 * rpc_music_list_cb_t，并携带用户上下文。ctx 由发起方分配，此处用完释放 */
typedef struct {
    rpc_music_list_cb_t cb;
    void *user_ctx;
} music_list_async_ctx_t;

static int music_list_async_adapter(ctrl_cmd_t *resp)
{
    music_list_async_ctx_t *ctx = (music_list_async_ctx_t *)resp->user_ctx;
    int result = resp->resp_event_status;

    if (!ctx) {
        ESP_LOGE(TAG, "music list async resp without ctx");
        return RPC_ERR_FAILURE;
    }

    ctx->cb(result,
            result == RPC_ERR_SUCCESS ? &resp->u.sd_music_list_resp : NULL,
            ctx->user_ctx);

    free(ctx);
    return RPC_ERR_SUCCESS;
}

int rpc_sd_music_list_async(storage_dev_type_t dev, uint32_t offset, uint32_t count,
                            rpc_music_list_cb_t cb, void *user_ctx)
{
    if (!cb) {
        ESP_LOGE(TAG, "Invalid cb parameter");
        return RPC_ERR_INVALID_PARAM;
    }

    music_list_async_ctx_t *ctx = (music_list_async_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return RPC_ERR_NO_MEMORY;
    }
    ctx->cb = cb;
    ctx->user_ctx = user_ctx;

    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    req->u.sd_music_list_req.dev = dev;
    req->u.sd_music_list_req.offset = offset;
    req->u.sd_music_list_req.count = count;
    req->rpc_rsp_cb = music_list_async_adapter;
    req->user_ctx = ctx;

    /* 不走 rpc_slaveif_*：它对异步恒返回 NULL，无法区分入队成败。
     * rpc_send_req 失败时已释放 req，这里只需收拾 ctx */
    req->msg_id = RPC_ID__Req_SdMusicList;
    if (rpc_send_req(req) != SUCCESS) {
        ESP_LOGE(TAG, "Failed to queue async music list req");
        free(ctx);
        return RPC_ERR_FAILURE;
    }

    return RPC_ERR_SUCCESS;
}

int rpc_music_mode_get(music_mode_t *mode_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!mode_out) {
        ESP_LOGE(TAG, "Invalid mode_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_music_mode_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get music mode response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *mode_out = resp->u.music_mode_get_resp.mode;
    }

    return rpc_rsp_callback(resp);
}

int rpc_music_mode_set(music_mode_t mode)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.music_mode_set_req.mode = mode;

    resp = rpc_slaveif_music_mode_set(req);
    return rpc_rsp_callback(resp);
}

int rpc_music_track_switch(bool next)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.music_track_switch_req.next = next;

    resp = rpc_slaveif_music_track_switch(req);
    return rpc_rsp_callback(resp);
}

int rpc_music_play_by_index(uint32_t index)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.music_play_by_index_req.index = index;

    resp = rpc_slaveif_play_music_by_index(req);
    return rpc_rsp_callback(resp);
}

int rpc_music_play_ctrl(music_play_ctrl_t ctrl)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.music_play_ctrl_req.ctrl = ctrl;

    resp = rpc_slaveif_music_play_ctrl(req);
    return rpc_rsp_callback(resp);
}

int rpc_music_is_playing(bool *is_playing_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!is_playing_out) {
        ESP_LOGE(TAG, "Invalid is_playing_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_music_is_playing(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get music playing state response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *is_playing_out = resp->u.music_is_playing_resp.is_playing;
    }

    return rpc_rsp_callback(resp);
}

int rpc_music_loop_mode_set(music_loop_mode_t loop_mode)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.music_loop_mode_set_req.loop_mode = loop_mode;

    resp = rpc_slaveif_music_loop_mode_set(req);
    return rpc_rsp_callback(resp);
}

int rpc_music_loop_mode_get(music_loop_mode_t *loop_mode_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!loop_mode_out) {
        ESP_LOGE(TAG, "Invalid loop_mode_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_music_loop_mode_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get music loop mode response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *loop_mode_out = resp->u.music_loop_mode_get_resp.loop_mode;
    }

    return rpc_rsp_callback(resp);
}

int rpc_music_volume_set(int32_t volume)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.music_volume_set_req.volume = volume;

    resp = rpc_slaveif_music_volume_set(req);
    return rpc_rsp_callback(resp);
}

int rpc_music_volume_get(int32_t *volume_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!volume_out) {
        ESP_LOGE(TAG, "Invalid volume_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_music_volume_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get music volume response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *volume_out = resp->u.music_volume_get_resp.volume;
    }

    return rpc_rsp_callback(resp);
}

int rpc_power_off(void)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = rpc_slaveif_power_off(req);
    return rpc_rsp_callback(resp);
}

int rpc_heartbeat(void)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    req->rsp_timeout_sec = 1;
    ctrl_cmd_t *resp = rpc_slaveif_heartbeat(req);
    return rpc_rsp_callback(resp);
}

int rpc_heartbeat_set(int32_t keep_alive_ms, int32_t *applied_keep_alive_ms_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    req->u.heartbeat_set_req.keep_alive_ms = keep_alive_ms;
    resp = rpc_slaveif_heartbeat_set(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get heartbeat set response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS && applied_keep_alive_ms_out) {
        *applied_keep_alive_ms_out = resp->u.heartbeat_set_resp.keep_alive_ms;
    }

    return rpc_rsp_callback(resp);
}

int rpc_reboot(void)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    req->rsp_timeout_sec = 2;
    ctrl_cmd_t *resp = rpc_slaveif_reboot(req);
    return rpc_rsp_callback(resp);
}

int rpc_sd_card_status(bool *inserted_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!inserted_out) {
        ESP_LOGE(TAG, "Invalid inserted_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_sd_card_status(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get SD card status response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *inserted_out = resp->u.sd_card_status_resp.inserted;
    }

    return rpc_rsp_callback(resp);
}

int rpc_bt_status(bool *connected_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!connected_out) {
        ESP_LOGE(TAG, "Invalid connected_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_bt_status(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get BT status response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *connected_out = resp->u.bt_status_resp.connected;
    }

    return rpc_rsp_callback(resp);
}

int rpc_time_get(time_get_resp_t *time_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!time_out) {
        ESP_LOGE(TAG, "Invalid time_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_time_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get time response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        time_out->year = resp->u.time_get_resp.year;
        time_out->month = resp->u.time_get_resp.month;
        time_out->day = resp->u.time_get_resp.day;
        time_out->hour = resp->u.time_get_resp.hour;
        time_out->minute = resp->u.time_get_resp.minute;
        time_out->second = resp->u.time_get_resp.second;
    }

    return rpc_rsp_callback(resp);
}

int rpc_time_set(uint32_t year, uint32_t month, uint32_t day,
                 uint32_t hour, uint32_t minute, uint32_t second)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();

    req->u.time_set_req.year = year;
    req->u.time_set_req.month = month;
    req->u.time_set_req.day = day;
    req->u.time_set_req.hour = hour;
    req->u.time_set_req.minute = minute;
    req->u.time_set_req.second = second;

    ctrl_cmd_t *resp = rpc_slaveif_time_set(req);
    return rpc_rsp_callback(resp);
}

int rpc_bt_disconnect_ex(bool keep_bond)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    req->u.bt_disconnect_req.keep_bond = keep_bond;
    ctrl_cmd_t *resp = rpc_slaveif_bt_disconnect(req);
    return rpc_rsp_callback(resp);
}

int rpc_bt_disconnect(void)
{
    /* 老接口保持"断开并解绑"的原语义: keep_bond 缺省即 false, 从机不认这个
     * 字段时走的也是同一条路径。 */
    return rpc_bt_disconnect_ex(false);
}

int rpc_bt_work_mode_set(bt_work_mode_t mode)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    req->u.bt_work_mode_set_req.mode = mode;
    ctrl_cmd_t *resp = rpc_slaveif_bt_work_mode_set(req);
    return rpc_rsp_callback(resp);
}

int rpc_bt_work_mode_get(bt_work_mode_t *mode_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!mode_out) {
        ESP_LOGE(TAG, "Invalid mode_out parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_bt_work_mode_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get BT work mode response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *mode_out = resp->u.bt_work_mode_get_resp.mode;
    }

    return rpc_rsp_callback(resp);
}

int rpc_bt_name_set(const char *name)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();

    if (!name) {
        ESP_LOGE(TAG, "Invalid name parameter");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    strncpy(req->u.bt_name_set_req.name, name, sizeof(req->u.bt_name_set_req.name) - 1);
#pragma GCC diagnostic pop

    ctrl_cmd_t *resp = rpc_slaveif_bt_name_set(req);
    return rpc_rsp_callback(resp);
}

int rpc_bt_name_get(char *name_out, size_t name_out_size)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!name_out || name_out_size == 0) {
        ESP_LOGE(TAG, "Invalid bt name output buffer");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    name_out[0] = '\0';
    resp = rpc_slaveif_bt_name_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get BT name response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        size_t copy_len = resp->u.bt_name_get_resp.name_len;
        if (copy_len >= name_out_size) {
            copy_len = name_out_size - 1;
        }
        memcpy(name_out, resp->u.bt_name_get_resp.name, copy_len);
        name_out[copy_len] = '\0';
    }

    return rpc_rsp_callback(resp);
}

int rpc_bt_conn_info_get(bt_connection_info_t *info_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!info_out) {
        ESP_LOGE(TAG, "Invalid BT connection info output");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    memset(info_out, 0, sizeof(*info_out));
    resp = rpc_slaveif_bt_conn_info_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get BT connection info response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *info_out = resp->u.bt_conn_info_get_resp;
    }

    return rpc_rsp_callback(resp);
}

int rpc_sco_ctrl(bool connect)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();

    req->u.sco_ctrl_req.connect = connect;
    ctrl_cmd_t *resp = rpc_slaveif_sco_ctrl(req);
    return rpc_rsp_callback(resp);
}

int rpc_alarm_set(const alarm_set_req_t *req_conf, uint32_t *index_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!req_conf) {
        ESP_LOGE(TAG, "Invalid alarm set request");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    req->u.alarm_set_req = *req_conf;
    resp = rpc_slaveif_alarm_set(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get alarm set response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS && index_out) {
        *index_out = resp->u.alarm_set_resp.index;
    }

    return rpc_rsp_callback(resp);
}

int rpc_alarm_cancel(uint32_t index, bool all, uint32_t *canceled_index_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    req->u.alarm_cancel_req.index = index;
    req->u.alarm_cancel_req.all = all;
    resp = rpc_slaveif_alarm_cancel(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get alarm cancel response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS && canceled_index_out) {
        *canceled_index_out = resp->u.alarm_cancel_resp.index;
    }

    return rpc_rsp_callback(resp);
}

int rpc_alarm_get(uint32_t index, alarm_get_resp_t *resp_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!resp_out) {
        ESP_LOGE(TAG, "Invalid alarm get output");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    memset(resp_out, 0, sizeof(*resp_out));
    req->u.alarm_get_req.index = index;
    resp = rpc_slaveif_alarm_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get alarm query response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *resp_out = resp->u.alarm_get_resp;
    }

    return rpc_rsp_callback(resp);
}

int rpc_button_config(const button_config_req_t *conf)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    if (!conf) {
        ESP_LOGE(TAG, "Invalid button config");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    req->u.button_config_req = *conf;
    resp = rpc_slaveif_button_config(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get button config response");
        return RPC_ERR_FAILURE;
    }

    return rpc_rsp_callback(resp);
}

int rpc_gpio_config(const gpio_config_req_t *conf, bool *level_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!conf) {
        ESP_LOGE(TAG, "Invalid gpio config");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    req->u.gpio_config_req = *conf;
    resp = rpc_slaveif_gpio_config(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get gpio config response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS && level_out) {
        *level_out = resp->u.gpio_level_resp.level;
    }

    return rpc_rsp_callback(resp);
}

int rpc_gpio_read(uint32_t io, bool *level_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!level_out) {
        ESP_LOGE(TAG, "Invalid gpio read output");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    req->u.gpio_read_req.io = io;
    resp = rpc_slaveif_gpio_read(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get gpio read response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *level_out = resp->u.gpio_level_resp.level;
    }

    return rpc_rsp_callback(resp);
}

int rpc_gpio_write(uint32_t io, bool level)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.gpio_write_req.io = io;
    req->u.gpio_write_req.level = level;
    resp = rpc_slaveif_gpio_write(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get gpio write response");
        return RPC_ERR_FAILURE;
    }

    return rpc_rsp_callback(resp);
}

int rpc_battery_get(battery_get_resp_t *info_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!info_out) {
        ESP_LOGE(TAG, "Invalid battery get output");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_battery_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get battery response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *info_out = resp->u.battery_get_resp;
    }

    return rpc_rsp_callback(resp);
}

int rpc_bt_switch(bool on)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.bt_switch_req.on = on;
    resp = rpc_slaveif_bt_switch(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get bt switch response");
        return RPC_ERR_FAILURE;
    }

    return rpc_rsp_callback(resp);
}

int rpc_firmware_version_get(firmware_version_resp_t *ver_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!ver_out) {
        ESP_LOGE(TAG, "Invalid firmware version output");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_firmware_version_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get firmware version response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *ver_out = resp->u.firmware_version_resp;
    }

    return rpc_rsp_callback(resp);
}

int rpc_music_playing_index_get(uint32_t *index_out)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;
    int result;

    if (!index_out) {
        ESP_LOGE(TAG, "Invalid playing index output");
        HOSTED_FREE(req);
        return RPC_ERR_INVALID_PARAM;
    }

    resp = rpc_slaveif_music_playing_index_get(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get playing index response");
        return RPC_ERR_FAILURE;
    }

    result = resp->resp_event_status;
    if (result == RPC_ERR_SUCCESS) {
        *index_out = resp->u.music_playing_index_resp.index;
    }

    return rpc_rsp_callback(resp);
}

int rpc_mic_data_ctrl(bool enable)
{
    ctrl_cmd_t *req = RPC_DEFAULT_REQ();
    ctrl_cmd_t *resp = NULL;

    req->u.mic_data_ctrl_req.enable = enable;
    resp = rpc_slaveif_mic_data_ctrl(req);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to get mic data ctrl response");
        return RPC_ERR_FAILURE;
    }

    return rpc_rsp_callback(resp);
}

static int rpc_event_callback(ctrl_cmd_t * app_event)
{
	ESP_LOGV(TAG, "%u",app_event->msg_id);
	if (!app_event || (app_event->msg_type != RPC_TYPE__Event)) {
		if (app_event)
			ESP_LOGE(TAG, "Recvd msg [0x%x] is not event",app_event->msg_type);
		goto fail_parsing;
	}

	if ((app_event->msg_id <= RPC_ID__Evt_Base) ||
		(app_event->msg_id >= RPC_ID__Evt_Max)) {
		ESP_LOGE(TAG, "Event Msg ID[0x%x] is not correct",app_event->msg_id);
		goto fail_parsing;
	}
    
    switch (app_event->msg_id)
    {
    case RPC_ID__Evt_BTConnected:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_BT_CONNECTED,
                        &app_event->u.bt_connected_evt, sizeof(event_bt_connected_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_BTDisconnected:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_BT_DISCONNECTED,
                        &app_event->u.bt_disconnected_evt, sizeof(event_bt_disconnected_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_BTMusicPlay:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_BT_MUSIC_PLAY, NULL, 0, portMAX_DELAY);
        break;

    case RPC_ID__Evt_BTMusicPause:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_BT_MUSIC_PAUSE, NULL, 0, portMAX_DELAY);
        break;

    case RPC_ID__Evt_VolumeChanged:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_VOLUME_CHANGED,
                        &app_event->u.volume_changed_evt, sizeof(event_volume_changed_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_MusicInfoTitle:
        post_music_title_event(&app_event->u.music_info_title_evt);
        break;

    case RPC_ID__Evt_MusicInfoLyrc:
        post_music_lyrc_event(&app_event->u.music_info_lyrc_evt);
        break;

    case RPC_ID__Evt_MusicInfoTime:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_MUSIC_INFO_TIME,
                        &app_event->u.music_info_time_evt, sizeof(event_music_time_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_MusicModeChanged:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_MUSIC_MODE_CHANGED,
                        &app_event->u.music_mode_changed_evt, sizeof(event_music_mode_changed_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_AsrWord:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_ASR_WORD,
                        &app_event->u.asr_word.word, sizeof(event_asr_word_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_EmitterScanResult:
        post_emitter_scan_result_event(&app_event->u.emitter_scan_result_evt);
        break;

    case RPC_ID__Evt_StorageDevChanged:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_STORAGE_DEV_CHANGED,
                        &app_event->u.storage_dev_changed_evt, sizeof(event_storage_dev_changed_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_HfpStatus:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_HFP_STATUS,
                        &app_event->u.hfp_status_evt, sizeof(event_hfp_status_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_ScoStatus:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_SCO_STATUS,
                        &app_event->u.sco_status_evt, sizeof(event_sco_status_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_DoaAngle:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_DOA_ANGLE,
                        &app_event->u.doa_angle_evt, sizeof(event_doa_angle_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_AlarmFired:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_ALARM_FIRED,
                        &app_event->u.alarm_fired_evt, sizeof(event_alarm_fired_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_Button:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_BUTTON,
                        &app_event->u.button_evt, sizeof(event_button_t), portMAX_DELAY);
        break;

    case RPC_ID__Evt_AsrWordEx:
        rpcp_event_post(RPC_VB_EVENT, VB_EVT_ASR_WORD_EX,
                        &app_event->u.asr_word_ex_evt, sizeof(event_asr_word_ex_t), portMAX_DELAY);
        break;

    default:
        ESP_LOGW(TAG, "Unhandled event: %u", app_event->msg_id);
        break;
    }
    
    CLEANUP_RPC(app_event);
	return SUCCESS;

fail_parsing:
	CLEANUP_RPC(app_event);
	return FAILURE;
}

int rpc_sen_audio_pcm(uint8_t *data, size_t len){
    serial_transfer_send_packed(PACK_TYPE_AUDIO, data, len);
    return 0;
}

int rpc_resister_event_callbacks(void){
    int ret = SUCCESS;
	int evt = 0;

    /* 「当前这首」缓存要在这里起来：esp_event 对同一事件按注册顺序派发，
     * 这个函数跑在驱动初始化阶段，早于任何板级/应用层订阅者，所以应用层
     * 收到事件时缓存必然已经是新值。顺序保证由驱动层给，不甩给调用方。 */
    rpc_music_now_init();

    event_callback_table_t events[] = {
        {RPC_ID__Evt_BTConnected, rpc_event_callback},
        {RPC_ID__Evt_BTDisconnected, rpc_event_callback},
        {RPC_ID__Evt_BTMusicPlay, rpc_event_callback},
        {RPC_ID__Evt_BTMusicPause, rpc_event_callback},
        {RPC_ID__Evt_VolumeChanged, rpc_event_callback},
        {RPC_ID__Evt_MusicInfoTitle, rpc_event_callback},
        {RPC_ID__Evt_MusicInfoLyrc, rpc_event_callback},
        {RPC_ID__Evt_MusicInfoTime, rpc_event_callback},
        {RPC_ID__Evt_MusicModeChanged, rpc_event_callback},
        {RPC_ID__Evt_AsrWord, rpc_event_callback},
        {RPC_ID__Evt_EmitterScanResult, rpc_event_callback},
        {RPC_ID__Evt_StorageDevChanged, rpc_event_callback},
        {RPC_ID__Evt_HfpStatus, rpc_event_callback},
        {RPC_ID__Evt_ScoStatus, rpc_event_callback},
        {RPC_ID__Evt_DoaAngle, rpc_event_callback},
        {RPC_ID__Evt_AlarmFired, rpc_event_callback},
        {RPC_ID__Evt_Button, rpc_event_callback},
        {RPC_ID__Evt_AsrWordEx, rpc_event_callback},
    };
    for (evt=0; evt<sizeof(events)/sizeof(event_callback_table_t); evt++) {
		if (CALLBACK_SET_SUCCESS != set_event_callback(events[evt].event, events[evt].fun) ) {
			ESP_LOGE(TAG, "event callback register failed for event[%u]", events[evt].event);
			ret = FAILURE;
			break;
		}
	}
	return ret;
}