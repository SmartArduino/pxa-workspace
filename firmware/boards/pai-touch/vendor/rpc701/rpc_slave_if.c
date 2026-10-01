#include "rpc_701_internal.h"
#include "esp_log.h"

static const char* TAG = "rpc_slave_if";

#define RPC_SEND_REQ(msGiD) do {                                                \
    assert(req);                                                                \
    req->msg_id = msGiD;                                                        \
    if(SUCCESS != rpc_send_req(req)) {                                          \
        ESP_LOGE(TAG,"Failed to send control req 0x%x\n", req->msg_id);         \
        return NULL;                                                            \
    }                                                                           \
} while(0);

#define RPC_DECODE_RSP_IF_NOT_ASYNC() do {                                      \
  if (req->rpc_rsp_cb)                                                          \
    return NULL;                                                                \
  return rpc_wait_and_parse_sync_resp(req);                                     \
} while(0);


ctrl_cmd_t *rpc_slaveif_vb_init(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_VbInit);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_emitter_connect(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_EmitterConnect);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_get_wake_word_list(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_GetAsrWords);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_emitter_start_scan(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_EmitterStartScan);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_asr_mic_mode_change(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_UserAsrMicModeChange);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_sd_music_count(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_SdMusicCount);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_sd_music_list(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_SdMusicList);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_mode_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicModeGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_mode_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicModeSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_track_switch(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicTrackSwitch);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_play_ctrl(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicPlayCtrl);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_is_playing(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicIsPlaying);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_loop_mode_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicLoopModeSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_loop_mode_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicLoopModeGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_volume_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicVolumeSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_volume_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicVolumeGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_play_music_by_index(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_PlayMusicByIndex);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_power_off(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_PowerOff);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_heartbeat(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_Heartbeat);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_heartbeat_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_HeartbeatSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_reboot(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_Reboot);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_sd_card_status(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_SdCardStatus);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_status(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtStatus);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_time_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_TimeGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_time_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_TimeSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_disconnect(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtDisconnect);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_work_mode_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtWorkModeSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_work_mode_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtWorkModeGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_name_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtNameSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_name_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtNameGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_conn_info_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtConnInfoGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_sco_ctrl(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_ScoCtrl);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_alarm_set(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_AlarmSet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_alarm_cancel(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_AlarmCancel);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_alarm_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_AlarmGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_button_config(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_ButtonConfig);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_gpio_config(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_GpioConfig);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_gpio_read(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_GpioRead);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_gpio_write(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_GpioWrite);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_battery_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BatteryGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_bt_switch(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_BtSwitch);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_firmware_version_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_FirmwareVersionGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_music_playing_index_get(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MusicPlayingIndexGet);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_mic_data_ctrl(ctrl_cmd_t *req){
    RPC_SEND_REQ(RPC_ID__Req_MicDataCtrl);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

