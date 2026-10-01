#include "rpc_701_internal.h"
#include "esp_log.h"

static const char* TAG = "rpc_evt";

int rpc_parse_evt(Rpc *rpc_msg, ctrl_cmd_t *app_ntfy)
{
	if (!rpc_msg || !app_ntfy) {
		ESP_LOGE(TAG, "NULL rpc event or App struct");
		goto fail_parse_rpc_msg;
	}

	app_ntfy->msg_type = RPC_TYPE__Event;
	app_ntfy->msg_id = rpc_msg->msg_id;
	app_ntfy->resp_event_status = SUCCESS;

	switch (rpc_msg->msg_id) {

	case RPC_ID__Evt_BTConnected: {
		ESP_LOGD(TAG, "EVENT: BT CONNECTED");
		RPC_FAIL_ON_NULL(evt_bt_connected);
		if (rpc_msg->evt_bt_connected->addr.len > 0 && rpc_msg->evt_bt_connected->addr.len <= 6) {
			memcpy(app_ntfy->u.bt_connected_evt.addr, rpc_msg->evt_bt_connected->addr.data,
			       rpc_msg->evt_bt_connected->addr.len);
		}
		app_ntfy->u.bt_connected_evt.is_sink = rpc_msg->evt_bt_connected->is_sink;
		if (rpc_msg->evt_bt_connected->name.len > 0) {
			size_t copy_len = rpc_msg->evt_bt_connected->name.len;
			if (copy_len >= sizeof(app_ntfy->u.bt_connected_evt.name))
				copy_len = sizeof(app_ntfy->u.bt_connected_evt.name) - 1;
			memcpy(app_ntfy->u.bt_connected_evt.name, rpc_msg->evt_bt_connected->name.data, copy_len);
			app_ntfy->u.bt_connected_evt.name[copy_len] = '\0';
			app_ntfy->u.bt_connected_evt.name_len = copy_len;
		}
		break;
	}

	case RPC_ID__Evt_BTDisconnected: {
		ESP_LOGD(TAG, "EVENT: BT DISCONNECTED");
		RPC_FAIL_ON_NULL(evt_bt_disconnected);
		if (rpc_msg->evt_bt_disconnected->addr.len > 0 && rpc_msg->evt_bt_disconnected->addr.len <= 6) {
			memcpy(app_ntfy->u.bt_disconnected_evt.addr, rpc_msg->evt_bt_disconnected->addr.data,
			       rpc_msg->evt_bt_disconnected->addr.len);
		}
		break;
	}

	case RPC_ID__Evt_BTMusicPlay:
	case RPC_ID__Evt_BTMusicPause:
		ESP_LOGD(TAG, "EVENT: Basic event (no data)");
		break;

	case RPC_ID__Evt_MusicInfoTitle: {
		ESP_LOGD(TAG, "EVENT: MUSIC INFO TITLE");
		RPC_FAIL_ON_NULL(evt_music_info_title);
		app_ntfy->u.music_info_title_evt.title_len = rpc_msg->evt_music_info_title->title.len;
		app_ntfy->u.music_info_title_evt.title = (char *)rpc_msg->evt_music_info_title->title.data;
		break;
	}

	case RPC_ID__Evt_MusicInfoLyrc: {
		ESP_LOGD(TAG, "EVENT: MUSIC INFO LYRC");
		RPC_FAIL_ON_NULL(evt_music_info_lyrc);
		app_ntfy->u.music_info_lyrc_evt.lyrc_len = rpc_msg->evt_music_info_lyrc->lyrc.len;
		app_ntfy->u.music_info_lyrc_evt.lyrc = (char *)rpc_msg->evt_music_info_lyrc->lyrc.data;
		break;
	}

	case RPC_ID__Evt_MusicInfoTime: {
		ESP_LOGD(TAG, "EVENT: MUSIC INFO TIME");
		RPC_FAIL_ON_NULL(evt_music_info_time);
		app_ntfy->u.music_info_time_evt.current_time_sec = rpc_msg->evt_music_info_time->current_time_sec;
		app_ntfy->u.music_info_time_evt.total_time_sec = rpc_msg->evt_music_info_time->total_time_sec;
		break;
	}

	case RPC_ID__Evt_VolumeChanged: {
		ESP_LOGD(TAG, "EVENT: VOLUME CHANGED");
		RPC_FAIL_ON_NULL(evt_volume_changed);
		app_ntfy->u.volume_changed_evt.volume = rpc_msg->evt_volume_changed->volume;
		break;
	}

	case RPC_ID__Evt_MusicModeChanged: {
		ESP_LOGD(TAG, "EVENT: MUSIC MODE CHANGED");
		RPC_FAIL_ON_NULL(evt_music_mode_changed);
		app_ntfy->u.music_mode_changed_evt.mode = rpc_msg->evt_music_mode_changed->mode;
		break;
	}

	case RPC_ID__Evt_AsrWord:{
		RPC_FAIL_ON_NULL(evt_asr_word)
		rpc_copy_pb_str(app_ntfy->u.asr_word.word, sizeof(app_ntfy->u.asr_word.word),
		                &app_ntfy->u.asr_word.word_len, &rpc_msg->evt_asr_word->words);
		ESP_LOGW(TAG, "EVENT: ASR WORD:%s", app_ntfy->u.asr_word.word);
		break;
	}

	case RPC_ID__Evt_EmitterScanResult: {
		ESP_LOGD(TAG, "EVENT: EMITTER SCAN RESULT");
		RPC_FAIL_ON_NULL(evt_emitter_scan_result);
		app_ntfy->u.emitter_scan_result_evt.name_len = rpc_msg->evt_emitter_scan_result->name.len;
		app_ntfy->u.emitter_scan_result_evt.name = (char *)rpc_msg->evt_emitter_scan_result->name.data;
		app_ntfy->u.emitter_scan_result_evt.addr_len = rpc_msg->evt_emitter_scan_result->addr.len;
		app_ntfy->u.emitter_scan_result_evt.addr = (char *)rpc_msg->evt_emitter_scan_result->addr.data;
		app_ntfy->u.emitter_scan_result_evt.dev_class = rpc_msg->evt_emitter_scan_result->dev_class;
		app_ntfy->u.emitter_scan_result_evt.rssi = rpc_msg->evt_emitter_scan_result->rssi;
		break;
	}

	case RPC_ID__Evt_StorageDevChanged: {
		ESP_LOGD(TAG, "EVENT: STORAGE DEV CHANGED");
		RPC_FAIL_ON_NULL(evt_storage_dev_changed);
		app_ntfy->u.storage_dev_changed_evt.dev = rpc_msg->evt_storage_dev_changed->dev;
		app_ntfy->u.storage_dev_changed_evt.action = rpc_msg->evt_storage_dev_changed->action;
		break;
	}

	case RPC_ID__Evt_HfpStatus: {
		ESP_LOGD(TAG, "EVENT: HFP STATUS");
		RPC_FAIL_ON_NULL(evt_hfp_status);
		app_ntfy->u.hfp_status_evt.connected = rpc_msg->evt_hfp_status->connected;
		break;
	}

	case RPC_ID__Evt_ScoStatus: {
		ESP_LOGD(TAG, "EVENT: SCO STATUS");
		RPC_FAIL_ON_NULL(evt_sco_status);
		app_ntfy->u.sco_status_evt.connected = rpc_msg->evt_sco_status->connected;
		break;
	}

	case RPC_ID__Evt_DoaAngle: {
		ESP_LOGD(TAG, "EVENT: DOA ANGLE");
		RPC_FAIL_ON_NULL(evt_doa_angle);
		app_ntfy->u.doa_angle_evt.angle = rpc_msg->evt_doa_angle->angle;
		break;
	}

	case RPC_ID__Evt_AlarmFired: {
		ESP_LOGD(TAG, "EVENT: ALARM FIRED");
		RPC_FAIL_ON_NULL(evt_alarm_fired);
		app_ntfy->u.alarm_fired_evt.year = rpc_msg->evt_alarm_fired->year;
		app_ntfy->u.alarm_fired_evt.month = rpc_msg->evt_alarm_fired->month;
		app_ntfy->u.alarm_fired_evt.day = rpc_msg->evt_alarm_fired->day;
		app_ntfy->u.alarm_fired_evt.hour = rpc_msg->evt_alarm_fired->hour;
		app_ntfy->u.alarm_fired_evt.minute = rpc_msg->evt_alarm_fired->minute;
		app_ntfy->u.alarm_fired_evt.second = rpc_msg->evt_alarm_fired->second;
		app_ntfy->u.alarm_fired_evt.index = rpc_msg->evt_alarm_fired->index;
		break;
	}

	case RPC_ID__Evt_Button: {
		ESP_LOGD(TAG, "EVENT: BUTTON");
		RPC_FAIL_ON_NULL(evt_button);
		app_ntfy->u.button_evt.id = rpc_msg->evt_button->id;
		app_ntfy->u.button_evt.event = (rpc_button_event_t)rpc_msg->evt_button->event;
		app_ntfy->u.button_evt.click_cnt = rpc_msg->evt_button->click_cnt;
		app_ntfy->u.button_evt.scan_cnt = rpc_msg->evt_button->scan_cnt;
		break;
	}

	case RPC_ID__Evt_AsrWordEx: {
		RPC_FAIL_ON_NULL(evt_asr_word_ex);
		rpc_copy_pb_str(app_ntfy->u.asr_word_ex_evt.word, sizeof(app_ntfy->u.asr_word_ex_evt.word),
		                &app_ntfy->u.asr_word_ex_evt.word_len, &rpc_msg->evt_asr_word_ex->words);
		app_ntfy->u.asr_word_ex_evt.seq = rpc_msg->evt_asr_word_ex->seq;
		ESP_LOGD(TAG, "EVENT: ASR WORD EX:%s seq=%u", app_ntfy->u.asr_word_ex_evt.word,
		         (unsigned)app_ntfy->u.asr_word_ex_evt.seq);
		break;
	}

	default: {
		ESP_LOGE(TAG, "Invalid/unsupported event[%u] received",rpc_msg->msg_id);
		goto fail_parse_rpc_msg;
		break;
	}

	}

	return SUCCESS;

fail_parse_rpc_msg:
	app_ntfy->resp_event_status = FAILURE;
	return FAILURE;
}
