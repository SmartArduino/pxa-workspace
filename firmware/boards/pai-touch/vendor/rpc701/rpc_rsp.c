#include <inttypes.h>
#include "rpc_701_internal.h"
#include "esp_log.h"

static const char* TAG = "rpc_rsp";

#define RPC_ERR_IN_RESP(msGparaM)                                             \
    if (rpc_msg->msGparaM->resp) {                                            \
        app_resp->resp_event_status = rpc_msg->msGparaM->resp;                \
        ESP_LOGW(TAG, "Hosted RPC_Resp [0x%"PRIx16"], uid [%"PRIu32"], resp code [%"PRIi32"]", \
                app_resp->msg_id, app_resp->uid, app_resp->resp_event_status); \
        goto fail_parse_rpc_msg;                                              \
    }

#define RPC_RSP_COPY_BYTES(dst,src) {                                         \
    if (src.data && src.len) {                                                \
        memcpy(dst, src.data, src.len);                         \
    }                                                                         \
}

int rpc_parse_rsp(Rpc *rpc_msg, ctrl_cmd_t *app_resp)
{
	/* 1. Check non NULL */
	if (!rpc_msg || !app_resp) {
		ESP_LOGE(TAG, "NULL rpc resp or NULL App Resp");
		goto fail_parse_rpc_msg;
	}

	/* 2. update basic fields */
	app_resp->msg_type = RPC_TYPE__Resp;
	app_resp->msg_id = rpc_msg->msg_id;
	app_resp->uid = rpc_msg->uid;
	ESP_LOGD(TAG, " --> RPC_Resp [0x%x], uid %ld", app_resp->msg_id, app_resp->uid);

	/* 3. parse Rpc into ctrl_cmd_t */
	switch (rpc_msg->msg_id) {
        case RPC_ID__Resp_VbInit:
            RPC_FAIL_ON_NULL(resp_vb_init);
            RPC_ERR_IN_RESP(resp_vb_init);
            break;
        case RPC_ID__Resp_EmitterConnect:
            RPC_FAIL_ON_NULL(resp_emitter_connect);
            RPC_ERR_IN_RESP(resp_emitter_connect);
            break;
        case RPC_ID__Resp_GetAsrWords:{
            RPC_FAIL_ON_NULL(resp_get_asr_words);
            RPC_ERR_IN_RESP(resp_get_asr_words);
            size_t n_words = rpc_msg->resp_get_asr_words->n_words_list;
            if (n_words > 0) {
                app_resp->u.get_wake_word_list_resp.n_words = n_words;
                app_resp->u.get_wake_word_list_resp.words_list = (wake_word_info_t*)calloc(n_words, sizeof(wake_word_info_t));
                if (!app_resp->u.get_wake_word_list_resp.words_list) {
                    ESP_LOGE(TAG, "Failed to allocate memory for words_list");
                    app_resp->resp_event_status = RPC_ERR_NO_MEMORY;
                    goto fail_parse_rpc_msg;
                }
                for (size_t i = 0; i < n_words; i++) {
                    app_resp->u.get_wake_word_list_resp.words_list[i].type = rpc_msg->resp_get_asr_words->words_list[i]->type;
                    if (rpc_msg->resp_get_asr_words->words_list[i]->word_name.data) {
                        size_t copy_len = rpc_msg->resp_get_asr_words->words_list[i]->word_name.len;
                        if (copy_len > sizeof(app_resp->u.get_wake_word_list_resp.words_list[i].word_name) - 1) {
                            copy_len = sizeof(app_resp->u.get_wake_word_list_resp.words_list[i].word_name) - 1;
                        }
                        memcpy(app_resp->u.get_wake_word_list_resp.words_list[i].word_name,
                               rpc_msg->resp_get_asr_words->words_list[i]->word_name.data,
                               copy_len);
                        app_resp->u.get_wake_word_list_resp.words_list[i].word_name[copy_len] = '\0';
                    }
                }
                app_resp->app_free_buff_func = free;
                app_resp->app_free_buff_hdl = app_resp->u.get_wake_word_list_resp.words_list;
            }
            break;
        }
        case RPC_ID__Resp_UserAsrMicModeChange:
            RPC_FAIL_ON_NULL(resp_user_asr_mic_mode_change);
            RPC_ERR_IN_RESP(resp_user_asr_mic_mode_change);
            break;
        case RPC_ID__Resp_SdMusicCount:
            RPC_FAIL_ON_NULL(resp_sd_music_count);
            RPC_ERR_IN_RESP(resp_sd_music_count);
            app_resp->u.sd_music_count_resp.total = rpc_msg->resp_sd_music_count->total;
            break;
        case RPC_ID__Resp_SdMusicList:{
            RPC_FAIL_ON_NULL(resp_sd_music_list);
            RPC_ERR_IN_RESP(resp_sd_music_list);
            
            size_t n_files = rpc_msg->resp_sd_music_list->n_files;
            app_resp->u.sd_music_list_resp.resp = rpc_msg->resp_sd_music_list->resp;
            app_resp->u.sd_music_list_resp.offset = rpc_msg->resp_sd_music_list->offset;
            app_resp->u.sd_music_list_resp.count = rpc_msg->resp_sd_music_list->count;
            app_resp->u.sd_music_list_resp.total = rpc_msg->resp_sd_music_list->total;
            
            if (n_files > 0) {
                app_resp->u.sd_music_list_resp.n_files = n_files;
                app_resp->u.sd_music_list_resp.files = (sd_music_file_info_t*)calloc(n_files, sizeof(sd_music_file_info_t));
                if (!app_resp->u.sd_music_list_resp.files) {
                    ESP_LOGE(TAG, "Failed to allocate memory for music files list");
                    app_resp->resp_event_status = RPC_ERR_NO_MEMORY;
                    goto fail_parse_rpc_msg;
                }
                
                for (size_t i = 0; i < n_files; i++) {
                    app_resp->u.sd_music_list_resp.files[i].index = rpc_msg->resp_sd_music_list->files[i]->index;
                    
                    if (rpc_msg->resp_sd_music_list->files[i]->file_name.data) {
                        size_t copy_len = rpc_msg->resp_sd_music_list->files[i]->file_name.len;
                        if (copy_len > sizeof(app_resp->u.sd_music_list_resp.files[i].file_name) - 1) {
                            copy_len = sizeof(app_resp->u.sd_music_list_resp.files[i].file_name) - 1;
                        }
                        memcpy(app_resp->u.sd_music_list_resp.files[i].file_name,
                               rpc_msg->resp_sd_music_list->files[i]->file_name.data,
                               copy_len);
                        app_resp->u.sd_music_list_resp.files[i].file_name[copy_len] = '\0';
                        ESP_LOGI(TAG, "Parsed music file: index=%lu, name=%s", 
                                 (unsigned long)app_resp->u.sd_music_list_resp.files[i].index,
                                 app_resp->u.sd_music_list_resp.files[i].file_name);
                    }
                }
                
                app_resp->app_free_buff_func = free;
                app_resp->app_free_buff_hdl = app_resp->u.sd_music_list_resp.files;
            }
            break;
        }
        case RPC_ID__Resp_MusicModeGet:
            RPC_FAIL_ON_NULL(resp_music_mode_get);
            RPC_ERR_IN_RESP(resp_music_mode_get);
            app_resp->u.music_mode_get_resp.mode = (music_mode_t)rpc_msg->resp_music_mode_get->mode;
            break;
        case RPC_ID__Resp_MusicModeSet:
            RPC_FAIL_ON_NULL(resp_music_mode_set);
            RPC_ERR_IN_RESP(resp_music_mode_set);
            break;
        case RPC_ID__Resp_MusicTrackSwitch:
            RPC_FAIL_ON_NULL(resp_music_track_switch);
            RPC_ERR_IN_RESP(resp_music_track_switch);
            break;
        case RPC_ID__Resp_MusicPlayCtrl:
            RPC_FAIL_ON_NULL(resp_music_play_ctrl);
            RPC_ERR_IN_RESP(resp_music_play_ctrl);
            break;
        case RPC_ID__Resp_MusicIsPlaying:
            RPC_FAIL_ON_NULL(resp_music_is_playing);
            RPC_ERR_IN_RESP(resp_music_is_playing);
            app_resp->u.music_is_playing_resp.is_playing = rpc_msg->resp_music_is_playing->is_playing;
            break;
        case RPC_ID__Resp_MusicLoopModeSet:
            RPC_FAIL_ON_NULL(resp_music_loop_mode_set);
            RPC_ERR_IN_RESP(resp_music_loop_mode_set);
            break;
        case RPC_ID__Resp_MusicLoopModeGet:
            RPC_FAIL_ON_NULL(resp_music_loop_mode_get);
            RPC_ERR_IN_RESP(resp_music_loop_mode_get);
            app_resp->u.music_loop_mode_get_resp.loop_mode = (music_loop_mode_t)rpc_msg->resp_music_loop_mode_get->loop_mode;
            break;
        case RPC_ID__Resp_MusicVolumeSet:
            RPC_FAIL_ON_NULL(resp_music_volume_set);
            RPC_ERR_IN_RESP(resp_music_volume_set);
            break;
        case RPC_ID__Resp_MusicVolumeGet:
            RPC_FAIL_ON_NULL(resp_music_volume_get);
            RPC_ERR_IN_RESP(resp_music_volume_get);
            app_resp->u.music_volume_get_resp.volume = rpc_msg->resp_music_volume_get->volume;
            break;
        case RPC_ID__Resp_PlayMusicByIndex:
            RPC_FAIL_ON_NULL(resp_play_music_by_index);
            RPC_ERR_IN_RESP(resp_play_music_by_index);
            break;
        case RPC_ID__Resp_PowerOff:
            RPC_FAIL_ON_NULL(resp_power_off);
            RPC_ERR_IN_RESP(resp_power_off);
            break;
        case RPC_ID__Resp_Heartbeat:
            RPC_FAIL_ON_NULL(resp_heartbeat);
            RPC_ERR_IN_RESP(resp_heartbeat);
            break;
        case RPC_ID__Resp_HeartbeatSet:
            RPC_FAIL_ON_NULL(resp_heartbeat_set);
            RPC_ERR_IN_RESP(resp_heartbeat_set);
            app_resp->u.heartbeat_set_resp.keep_alive_ms = rpc_msg->resp_heartbeat_set->keep_alive_ms;
            break;
        case RPC_ID__Resp_Reboot:
            RPC_FAIL_ON_NULL(resp_reboot);
            RPC_ERR_IN_RESP(resp_reboot);
            break;
        case RPC_ID__Resp_SdCardStatus:
            RPC_FAIL_ON_NULL(resp_sd_card_status);
            RPC_ERR_IN_RESP(resp_sd_card_status);
            app_resp->u.sd_card_status_resp.inserted = rpc_msg->resp_sd_card_status->inserted;
            break;
        case RPC_ID__Resp_BtStatus:
            RPC_FAIL_ON_NULL(resp_bt_status);
            RPC_ERR_IN_RESP(resp_bt_status);
            app_resp->u.bt_status_resp.connected = rpc_msg->resp_bt_status->connected;
            break;
        case RPC_ID__Resp_TimeGet:
            RPC_FAIL_ON_NULL(resp_time_get);
            RPC_ERR_IN_RESP(resp_time_get);
            app_resp->u.time_get_resp.year = rpc_msg->resp_time_get->year;
            app_resp->u.time_get_resp.month = rpc_msg->resp_time_get->month;
            app_resp->u.time_get_resp.day = rpc_msg->resp_time_get->day;
            app_resp->u.time_get_resp.hour = rpc_msg->resp_time_get->hour;
            app_resp->u.time_get_resp.minute = rpc_msg->resp_time_get->minute;
            app_resp->u.time_get_resp.second = rpc_msg->resp_time_get->second;
            break;
        case RPC_ID__Resp_TimeSet:
            RPC_FAIL_ON_NULL(resp_time_set);
            RPC_ERR_IN_RESP(resp_time_set);
            break;
        case RPC_ID__Resp_BtDisconnect:
            RPC_FAIL_ON_NULL(resp_bt_disconnect);
            RPC_ERR_IN_RESP(resp_bt_disconnect);
            break;
        case RPC_ID__Resp_BtWorkModeSet:
            RPC_FAIL_ON_NULL(resp_bt_work_mode_set);
            RPC_ERR_IN_RESP(resp_bt_work_mode_set);
            break;
        case RPC_ID__Resp_BtWorkModeGet:
            RPC_FAIL_ON_NULL(resp_bt_work_mode_get);
            RPC_ERR_IN_RESP(resp_bt_work_mode_get);
            app_resp->u.bt_work_mode_get_resp.mode = (bt_work_mode_t)rpc_msg->resp_bt_work_mode_get->mode;
            break;
        case RPC_ID__Resp_BtNameSet:
            RPC_FAIL_ON_NULL(resp_bt_name_set);
            RPC_ERR_IN_RESP(resp_bt_name_set);
            break;
        case RPC_ID__Resp_BtNameGet:
            RPC_FAIL_ON_NULL(resp_bt_name_get);
            RPC_ERR_IN_RESP(resp_bt_name_get);
            if (rpc_msg->resp_bt_name_get->name.data) {
                size_t copy_len = rpc_msg->resp_bt_name_get->name.len;
                if (copy_len > sizeof(app_resp->u.bt_name_get_resp.name) - 1) {
                    copy_len = sizeof(app_resp->u.bt_name_get_resp.name) - 1;
                }
                memcpy(app_resp->u.bt_name_get_resp.name,
                       rpc_msg->resp_bt_name_get->name.data,
                       copy_len);
                app_resp->u.bt_name_get_resp.name[copy_len] = '\0';
                app_resp->u.bt_name_get_resp.name_len = copy_len;
            }
            break;
        case RPC_ID__Resp_BtConnInfoGet:{
            RPC_FAIL_ON_NULL(resp_bt_conn_info_get);
            RPC_ERR_IN_RESP(resp_bt_conn_info_get);
            BtConnectionInfo *info = rpc_msg->resp_bt_conn_info_get->info;
            if (!info) {
                ESP_LOGE(TAG, "BT connection info response missing info");
                app_resp->resp_event_status = RPC_ERR_DECODE_FAILED;
                goto fail_parse_rpc_msg;
            }

            app_resp->u.bt_conn_info_get_resp.connected = info->connected;
            if (info->addr.data && info->addr.len > 0) {
                size_t copy_len = info->addr.len;
                if (copy_len > sizeof(app_resp->u.bt_conn_info_get_resp.addr)) {
                    copy_len = sizeof(app_resp->u.bt_conn_info_get_resp.addr);
                }
                memcpy(app_resp->u.bt_conn_info_get_resp.addr, info->addr.data, copy_len);
                app_resp->u.bt_conn_info_get_resp.addr_len = copy_len;
            }
            if (info->name.data && info->name.len > 0) {
                size_t copy_len = info->name.len;
                if (copy_len > sizeof(app_resp->u.bt_conn_info_get_resp.name) - 1) {
                    copy_len = sizeof(app_resp->u.bt_conn_info_get_resp.name) - 1;
                }
                memcpy(app_resp->u.bt_conn_info_get_resp.name, info->name.data, copy_len);
                app_resp->u.bt_conn_info_get_resp.name[copy_len] = '\0';
                app_resp->u.bt_conn_info_get_resp.name_len = copy_len;
            }
            app_resp->u.bt_conn_info_get_resp.dev_class = info->dev_class;
            app_resp->u.bt_conn_info_get_resp.is_sink = info->is_sink;
            app_resp->u.bt_conn_info_get_resp.support_hfp = info->support_hfp;
            app_resp->u.bt_conn_info_get_resp.has_mic = info->has_mic;
            app_resp->u.bt_conn_info_get_resp.support_cvsd = info->support_cvsd;
            app_resp->u.bt_conn_info_get_resp.support_msbc = info->support_msbc;
            app_resp->u.bt_conn_info_get_resp.sco_connected = info->sco_connected;
            app_resp->u.bt_conn_info_get_resp.a2dp_status = (bt_a2dp_status_t)info->a2dp_status;
            app_resp->u.bt_conn_info_get_resp.a2dp_channel_up = info->a2dp_channel_up;
            app_resp->u.bt_conn_info_get_resp.hfp_slc_up = info->hfp_slc_up;
            break;
        }
        case RPC_ID__Resp_ScoCtrl:
            RPC_FAIL_ON_NULL(resp_sco_ctrl);
            RPC_ERR_IN_RESP(resp_sco_ctrl);
            break;
        case RPC_ID__Resp_AlarmSet:
            RPC_FAIL_ON_NULL(resp_alarm_set);
            RPC_ERR_IN_RESP(resp_alarm_set);
            app_resp->u.alarm_set_resp.index = rpc_msg->resp_alarm_set->index;
            break;
        case RPC_ID__Resp_AlarmCancel:
            RPC_FAIL_ON_NULL(resp_alarm_cancel);
            RPC_ERR_IN_RESP(resp_alarm_cancel);
            app_resp->u.alarm_cancel_resp.index = rpc_msg->resp_alarm_cancel->index;
            app_resp->u.alarm_cancel_resp.all = rpc_msg->resp_alarm_cancel->all;
            break;
        case RPC_ID__Resp_AlarmGet:
            RPC_FAIL_ON_NULL(resp_alarm_get);
            RPC_ERR_IN_RESP(resp_alarm_get);
            app_resp->u.alarm_get_resp.active = rpc_msg->resp_alarm_get->active;
            app_resp->u.alarm_get_resp.year = rpc_msg->resp_alarm_get->year;
            app_resp->u.alarm_get_resp.month = rpc_msg->resp_alarm_get->month;
            app_resp->u.alarm_get_resp.day = rpc_msg->resp_alarm_get->day;
            app_resp->u.alarm_get_resp.hour = rpc_msg->resp_alarm_get->hour;
            app_resp->u.alarm_get_resp.minute = rpc_msg->resp_alarm_get->minute;
            app_resp->u.alarm_get_resp.second = rpc_msg->resp_alarm_get->second;
            app_resp->u.alarm_get_resp.index = rpc_msg->resp_alarm_get->index;
            app_resp->u.alarm_get_resp.mode = rpc_msg->resp_alarm_get->mode;
            break;
        case RPC_ID__Resp_ButtonConfig:
            RPC_FAIL_ON_NULL(resp_button_config);
            RPC_ERR_IN_RESP(resp_button_config);
            break;
        case RPC_ID__Resp_GpioConfig:
            RPC_FAIL_ON_NULL(resp_gpio_config);
            RPC_ERR_IN_RESP(resp_gpio_config);
            app_resp->u.gpio_level_resp.level = rpc_msg->resp_gpio_config->level;
            break;
        case RPC_ID__Resp_GpioRead:
            RPC_FAIL_ON_NULL(resp_gpio_read);
            RPC_ERR_IN_RESP(resp_gpio_read);
            app_resp->u.gpio_level_resp.level = rpc_msg->resp_gpio_read->level;
            break;
        case RPC_ID__Resp_GpioWrite:
            RPC_FAIL_ON_NULL(resp_gpio_write);
            RPC_ERR_IN_RESP(resp_gpio_write);
            app_resp->u.gpio_level_resp.level = rpc_msg->resp_gpio_write->level;
            break;
        case RPC_ID__Resp_BatteryGet:
            RPC_FAIL_ON_NULL(resp_battery_get);
            RPC_ERR_IN_RESP(resp_battery_get);
            app_resp->u.battery_get_resp.percent = rpc_msg->resp_battery_get->percent;
            app_resp->u.battery_get_resp.voltage_mv = rpc_msg->resp_battery_get->voltage_mv;
            app_resp->u.battery_get_resp.charging = rpc_msg->resp_battery_get->charging;
            app_resp->u.battery_get_resp.voltage_rt_mv = rpc_msg->resp_battery_get->voltage_rt_mv;
            break;
        case RPC_ID__Resp_BtSwitch:
            RPC_FAIL_ON_NULL(resp_bt_switch);
            RPC_ERR_IN_RESP(resp_bt_switch);
            break;
        case RPC_ID__Resp_FirmwareVersionGet: {
            RPC_FAIL_ON_NULL(resp_firmware_version_get);
            RPC_ERR_IN_RESP(resp_firmware_version_get);
            RpcRespFirmwareVersionGet *fw = rpc_msg->resp_firmware_version_get;
            firmware_version_resp_t *out = &app_resp->u.firmware_version_resp;
            rpc_copy_pb_str(out->name, sizeof(out->name), &out->name_len, &fw->name);
            rpc_copy_pb_str(out->version, sizeof(out->version), &out->version_len, &fw->version);
            rpc_copy_pb_str(out->build_date, sizeof(out->build_date), &out->build_date_len, &fw->build_date);
            rpc_copy_pb_str(out->build_time, sizeof(out->build_time), &out->build_time_len, &fw->build_time);
            /* MAC 是二进制不是字符串，走 bytes 版：char 版会为 NUL 让位，
             * 把 6 字节砍成 5。对端不给时 mac_len 归 0、mac 清零。 */
            rpc_copy_pb_bytes(out->mac, sizeof(out->mac), &out->mac_len, &fw->mac);
            break;
        }
        case RPC_ID__Resp_MusicPlayingIndexGet:
            RPC_FAIL_ON_NULL(resp_music_playing_index_get);
            RPC_ERR_IN_RESP(resp_music_playing_index_get);
            app_resp->u.music_playing_index_resp.index = rpc_msg->resp_music_playing_index_get->index;
            break;
        case RPC_ID__Resp_MicDataCtrl:
            RPC_FAIL_ON_NULL(resp_mic_data_ctrl);
            RPC_ERR_IN_RESP(resp_mic_data_ctrl);
            break;
        case RPC_ID__Resp_OtaBegin:
        case RPC_ID__Resp_OtaChunk:
        case RPC_ID__Resp_OtaCommit:
        case RPC_ID__Resp_OtaAbort:
        case RPC_ID__Resp_OtaStatusQuery:
            return rpc_parse_ota_rsp(rpc_msg, app_resp);
        default:
            ESP_LOGW(TAG, "No parser implemented for RPC Resp [0x%x]", rpc_msg->msg_id);
            goto fail_parse_rpc_msg;
            break;
    }
    app_resp->resp_event_status = SUCCESS;
	return SUCCESS;

	/* 5. Free up buffers in failure cases */
fail_parse_rpc_msg:
	return SUCCESS;
}
