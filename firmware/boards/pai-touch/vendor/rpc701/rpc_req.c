#include "rpc_701_internal.h"
#include "esp_log.h"

static const char* TAG = "rpc_req";

#define ADD_RPC_BUFF_TO_FREE_LATER(BuFf) {                                      \
	assert((app_req->n_rpc_free_buff_hdls+1)<=MAX_FREE_BUFF_HANDLES);           \
	app_req->rpc_free_buff_hdls[app_req->n_rpc_free_buff_hdls++] = BuFf;        \
}

#define RPC_ALLOC_ASSIGN(TyPe,MsG_StRuCt,InItFuNc)                            \
    TyPe *req_payload = (TyPe *)                                              \
        calloc(1, sizeof(TyPe));                                \
    if (!req_payload) {                                                       \
        ESP_LOGE(TAG, "Failed to allocate memory for req->%s\n",#MsG_StRuCt);     \
        *failure_status = RPC_ERR_NO_MEMORY;                              \
		return FAILURE;                                                       \
    }                                                                         \
    req->MsG_StRuCt = req_payload;                                             \
	InItFuNc(req_payload);                                                    \
    ADD_RPC_BUFF_TO_FREE_LATER((uint8_t*)req_payload);

//TODO: How this is different in slave_control.c
#define RPC_ALLOC_ELEMENT(TyPe,MsG_StRuCt,InIt_FuN) {                         \
    TyPe *NeW_AllocN = (TyPe *) calloc(1, sizeof(TyPe));        \
    if (!NeW_AllocN) {                                                        \
        ESP_LOGE(TAG, "Failed to allocate memory for req->%s\n",#MsG_StRuCt);     \
        *failure_status = RPC_ERR_NO_MEMORY;                              \
		return FAILURE;                                                       \
    }                                                                         \
    ADD_RPC_BUFF_TO_FREE_LATER((uint8_t*)NeW_AllocN);                         \
    MsG_StRuCt = NeW_AllocN;                                                  \
    InIt_FuN(MsG_StRuCt);                                                     \
}


int compose_rpc_req(Rpc *req, ctrl_cmd_t *app_req, int32_t *failure_status)
{
    switch (req->msg_id)
    {
    case RPC_ID__Req_GetAsrWords:
    case RPC_ID__Req_EmitterStartScan:
    case RPC_ID__Req_MusicModeGet:
    case RPC_ID__Req_MusicIsPlaying:
    case RPC_ID__Req_MusicLoopModeGet:
    case RPC_ID__Req_MusicVolumeGet:
    case RPC_ID__Req_PowerOff:
    case RPC_ID__Req_Heartbeat:
    case RPC_ID__Req_Reboot:
    case RPC_ID__Req_SdCardStatus:
    case RPC_ID__Req_BtStatus:
    case RPC_ID__Req_TimeGet:
    case RPC_ID__Req_BtWorkModeGet:
    case RPC_ID__Req_BtNameGet:
    case RPC_ID__Req_BtConnInfoGet:
    case RPC_ID__Req_BatteryGet:
    case RPC_ID__Req_MusicPlayingIndexGet:
        break;
    case RPC_ID__Req_FirmwareVersionGet:
        // Although the request has no fields, protobuf still needs this nested
        // message set to select the oneof member on the 701 side.
        RPC_ALLOC_ASSIGN(RpcReqFirmwareVersionGet, req_firmware_version_get,
                         rpc__req__firmware_version_get__init);
        break;
    case RPC_ID__Req_EmitterConnect:{
        emitter_connect_conf_t *emitter_connect_conf = &app_req->u.emitter_connect_conf;
        RPC_ALLOC_ASSIGN(RpcReqEmitterConnect, req_emitter_connect, rpc__req__emitter_connect__init);
        RPC_REQ_COPY_BYTES(req->req_emitter_connect->addr, emitter_connect_conf->addr, sizeof(emitter_connect_conf->addr));
        break;
    }
    case RPC_ID__Req_VbInit:{
        vb_init_conf_t *vb_init_conf = &app_req->u.vb_init_conf;
        RPC_ALLOC_ASSIGN(RpcReqVbInit, req_vb_init, rpc__req__vb_init__init);
        RPC_REQ_COPY_STR(req->req_vb_init->bt_name, vb_init_conf->bt_name, sizeof(vb_init_conf->bt_name));
        req->req_vb_init->keep_alive_ms = vb_init_conf->keep_alive_ms;
        req->req_vb_init->pa_io = vb_init_conf->pa_io;
        req->req_vb_init->pa_en_level = vb_init_conf->pa_en_level;
        req->req_vb_init->coder_fmt = vb_init_conf->coder_fmt;
        req->req_vb_init->decoder_fmt = vb_init_conf->decoder_fmt;
        req->req_vb_init->debug_uart_io = vb_init_conf->debug_uart_io;
        break;
    }
    case RPC_ID__Req_UserAsrMicModeChange:{
        asr_mic_mode_conf_t *asr_mic_mode_conf = &app_req->u.asr_mic_mode_conf;
        RPC_ALLOC_ASSIGN(RpcReqUserAsrMicModeChange, req_user_asr_mic_mode_change, rpc__req__user_asr_mic_mode_change__init);
        req->req_user_asr_mic_mode_change->mode = asr_mic_mode_conf->mode;
        req->req_user_asr_mic_mode_change->gain = asr_mic_mode_conf->gain;
        break;
    }
    case RPC_ID__Req_SdMusicCount:{
        sd_music_count_req_t *music_count_req = &app_req->u.sd_music_count_req;
        RPC_ALLOC_ASSIGN(RpcReqSdMusicCount, req_sd_music_count, rpc__req__sd_music_count__init);
        req->req_sd_music_count->dev = music_count_req->dev;
        break;
    }
    case RPC_ID__Req_SdMusicList:{
        sd_music_list_req_t *music_list_req = &app_req->u.sd_music_list_req;
        RPC_ALLOC_ASSIGN(RpcReqSdMusicList, req_sd_music_list, rpc__req__sd_music_list__init);
        req->req_sd_music_list->dev = music_list_req->dev;
        req->req_sd_music_list->offset = music_list_req->offset;
        req->req_sd_music_list->count = music_list_req->count;
        break;
    }
    case RPC_ID__Req_MusicModeSet:{
        music_mode_set_req_t *music_mode_set_req = &app_req->u.music_mode_set_req;
        RPC_ALLOC_ASSIGN(RpcReqMusicModeSet, req_music_mode_set, rpc__req__music_mode_set__init);
        req->req_music_mode_set->mode = (MusicMode)music_mode_set_req->mode;
        break;
    }
    case RPC_ID__Req_MusicTrackSwitch:{
        music_track_switch_req_t *music_track_switch_req = &app_req->u.music_track_switch_req;
        RPC_ALLOC_ASSIGN(RpcReqMusicTrackSwitch, req_music_track_switch, rpc__req__music_track_switch__init);
        req->req_music_track_switch->next = music_track_switch_req->next;
        break;
    }
    case RPC_ID__Req_MusicPlayCtrl:{
        music_play_ctrl_req_t *music_play_ctrl_req = &app_req->u.music_play_ctrl_req;
        RPC_ALLOC_ASSIGN(RpcReqMusicPlayCtrl, req_music_play_ctrl, rpc__req__music_play_ctrl__init);
        req->req_music_play_ctrl->ctrl = (MusicPlayCtrl)music_play_ctrl_req->ctrl;
        break;
    }
    case RPC_ID__Req_MusicLoopModeSet:{
        music_loop_mode_set_req_t *music_loop_mode_set_req = &app_req->u.music_loop_mode_set_req;
        RPC_ALLOC_ASSIGN(RpcReqMusicLoopModeSet, req_music_loop_mode_set, rpc__req__music_loop_mode_set__init);
        req->req_music_loop_mode_set->loop_mode = (MusicLoopMode)music_loop_mode_set_req->loop_mode;
        break;
    }
    case RPC_ID__Req_MusicVolumeSet:{
        music_volume_set_req_t *music_volume_set_req = &app_req->u.music_volume_set_req;
        RPC_ALLOC_ASSIGN(RpcReqMusicVolumeSet, req_music_volume_set, rpc__req__music_volume_set__init);
        req->req_music_volume_set->volume = music_volume_set_req->volume;
        break;
    }
    case RPC_ID__Req_PlayMusicByIndex:{
        music_play_by_index_req_t *music_play_by_index_req = &app_req->u.music_play_by_index_req;
        RPC_ALLOC_ASSIGN(RpcReqPlayMusicByIndex, req_play_music_by_index, rpc__req__play_music_by_index__init);
        req->req_play_music_by_index->index = music_play_by_index_req->index;
        break;
    }
    case RPC_ID__Req_HeartbeatSet:{
        heartbeat_set_req_t *heartbeat_set_req = &app_req->u.heartbeat_set_req;
        RPC_ALLOC_ASSIGN(RpcReqHeartbeatSet, req_heartbeat_set, rpc__req__heartbeat_set__init);
        req->req_heartbeat_set->keep_alive_ms = heartbeat_set_req->keep_alive_ms;
        break;
    }
    case RPC_ID__Req_TimeSet:{
        time_set_req_t *time_set_req = &app_req->u.time_set_req;
        RPC_ALLOC_ASSIGN(RpcReqTimeSet, req_time_set, rpc__req__time_set__init);
        req->req_time_set->year = time_set_req->year;
        req->req_time_set->month = time_set_req->month;
        req->req_time_set->day = time_set_req->day;
        req->req_time_set->hour = time_set_req->hour;
        req->req_time_set->minute = time_set_req->minute;
        req->req_time_set->second = time_set_req->second;
        break;
    }
    case RPC_ID__Req_BtDisconnect:{
        bt_disconnect_req_t *bt_disconnect_req = &app_req->u.bt_disconnect_req;
        RPC_ALLOC_ASSIGN(RpcReqBtDisconnect, req_bt_disconnect, rpc__req__bt_disconnect__init);
        req->req_bt_disconnect->keep_bond = bt_disconnect_req->keep_bond;
        break;
    }
    case RPC_ID__Req_BtWorkModeSet:{
        bt_work_mode_set_req_t *bt_work_mode_set_req = &app_req->u.bt_work_mode_set_req;
        RPC_ALLOC_ASSIGN(RpcReqBtWorkModeSet, req_bt_work_mode_set, rpc__req__bt_work_mode_set__init);
        req->req_bt_work_mode_set->mode = (BtWorkMode)bt_work_mode_set_req->mode;
        break;
    }
    case RPC_ID__Req_BtNameSet:{
        bt_name_set_req_t *bt_name_set_req = &app_req->u.bt_name_set_req;
        RPC_ALLOC_ASSIGN(RpcReqBtNameSet, req_bt_name_set, rpc__req__bt_name_set__init);
        RPC_REQ_COPY_STR(req->req_bt_name_set->name, bt_name_set_req->name, sizeof(bt_name_set_req->name));
        break;
    }
    case RPC_ID__Req_ScoCtrl:{
        sco_ctrl_req_t *sco_ctrl_req = &app_req->u.sco_ctrl_req;
        RPC_ALLOC_ASSIGN(RpcReqScoCtrl, req_sco_ctrl, rpc__req__sco_ctrl__init);
        req->req_sco_ctrl->connect = sco_ctrl_req->connect;
        break;
    }
    case RPC_ID__Req_AlarmSet:{
        alarm_set_req_t *alarm_set_req = &app_req->u.alarm_set_req;
        RPC_ALLOC_ASSIGN(RpcReqAlarmSet, req_alarm_set, rpc__req__alarm_set__init);
        req->req_alarm_set->year = alarm_set_req->year;
        req->req_alarm_set->month = alarm_set_req->month;
        req->req_alarm_set->day = alarm_set_req->day;
        req->req_alarm_set->hour = alarm_set_req->hour;
        req->req_alarm_set->minute = alarm_set_req->minute;
        req->req_alarm_set->second = alarm_set_req->second;
        req->req_alarm_set->index = alarm_set_req->index;
        req->req_alarm_set->mode = alarm_set_req->mode;
        req->req_alarm_set->sw = alarm_set_req->sw;
        break;
    }
    case RPC_ID__Req_AlarmCancel:{
        alarm_cancel_req_t *alarm_cancel_req = &app_req->u.alarm_cancel_req;
        RPC_ALLOC_ASSIGN(RpcReqAlarmCancel, req_alarm_cancel, rpc__req__alarm_cancel__init);
        req->req_alarm_cancel->index = alarm_cancel_req->index;
        req->req_alarm_cancel->all = alarm_cancel_req->all;
        break;
    }
    case RPC_ID__Req_AlarmGet:{
        alarm_get_req_t *alarm_get_req = &app_req->u.alarm_get_req;
        RPC_ALLOC_ASSIGN(RpcReqAlarmGet, req_alarm_get, rpc__req__alarm_get__init);
        req->req_alarm_get->index = alarm_get_req->index;
        break;
    }
    case RPC_ID__Req_ButtonConfig:{
        button_config_req_t *button_config_req = &app_req->u.button_config_req;
        RPC_ALLOC_ASSIGN(RpcReqButtonConfig, req_button_config, rpc__req__button_config__init);
        req->req_button_config->id = button_config_req->id;
        req->req_button_config->io = button_config_req->io;
        req->req_button_config->pressed_logic_level = button_config_req->pressed_logic_level;
        req->req_button_config->short_press_ms = button_config_req->short_press_ms;
        req->req_button_config->long_press_ms = button_config_req->long_press_ms;
        req->req_button_config->long_hold_ms = button_config_req->long_hold_ms;
        req->req_button_config->enable = button_config_req->enable;
        break;
    }
    case RPC_ID__Req_GpioConfig:{
        gpio_config_req_t *gpio_config_req = &app_req->u.gpio_config_req;
        RPC_ALLOC_ASSIGN(RpcReqGpioConfig, req_gpio_config, rpc__req__gpio_config__init);
        req->req_gpio_config->io = gpio_config_req->io;
        req->req_gpio_config->direction = (GpioDirection)gpio_config_req->direction;
        req->req_gpio_config->pull_up = gpio_config_req->pull_up;
        req->req_gpio_config->pull_down = gpio_config_req->pull_down;
        req->req_gpio_config->die = gpio_config_req->die;
        break;
    }
    case RPC_ID__Req_GpioRead:{
        gpio_read_req_t *gpio_read_req = &app_req->u.gpio_read_req;
        RPC_ALLOC_ASSIGN(RpcReqGpioRead, req_gpio_read, rpc__req__gpio_read__init);
        req->req_gpio_read->io = gpio_read_req->io;
        break;
    }
    case RPC_ID__Req_GpioWrite:{
        gpio_write_req_t *gpio_write_req = &app_req->u.gpio_write_req;
        RPC_ALLOC_ASSIGN(RpcReqGpioWrite, req_gpio_write, rpc__req__gpio_write__init);
        req->req_gpio_write->io = gpio_write_req->io;
        req->req_gpio_write->level = gpio_write_req->level;
        break;
    }
    case RPC_ID__Req_BtSwitch:{
        bt_switch_req_t *bt_switch_req = &app_req->u.bt_switch_req;
        RPC_ALLOC_ASSIGN(RpcReqBtSwitch, req_bt_switch, rpc__req__bt_switch__init);
        req->req_bt_switch->on = bt_switch_req->on;
        break;
    }
    case RPC_ID__Req_MicDataCtrl:{
        mic_data_ctrl_req_t *mic_data_ctrl_req = &app_req->u.mic_data_ctrl_req;
        RPC_ALLOC_ASSIGN(RpcReqMicDataCtrl, req_mic_data_ctrl, rpc__req__mic_data_ctrl__init);
        req->req_mic_data_ctrl->enable = mic_data_ctrl_req->enable;
        break;
    }
    case RPC_ID__Req_OtaBegin:
    case RPC_ID__Req_OtaChunk:
    case RPC_ID__Req_OtaCommit:
    case RPC_ID__Req_OtaAbort:
    case RPC_ID__Req_OtaStatusQuery:
        return rpc_compose_ota_req(req, app_req, failure_status);
    default:{
        *failure_status = RPC_ERR_UNSUPPORTED_MSG;
        ESP_LOGE(TAG, "Unsupported RPC Req[%u]",req->msg_id);
        return FAILURE;
        break;
    }
    }
    return SUCCESS;
}
