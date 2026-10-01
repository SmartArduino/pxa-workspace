#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "rpc_701_internal.h"
#include <errno.h>
#include "esp_timer.h"

#define RPC_TX_QUEUE_SIZE        5
#define RPC_RX_QUEUE_SIZE        3
#define RPC_TASK_STACK_SIZE   4096
#define RPC_TASK_PRIORITY     23
#define AUDIO_FRAME_BUF_SIZE  (1 * 1024)  // 16KB for OPUS frames

static const char* TAG = "RPC_701";

static queue_handle_t s_tx_queue = NULL;
static queue_handle_t s_rx_queue = NULL;
static semaphore_handle_t s_tx_sem = NULL;
static thread_handle_t s_tx_task_handle = NULL;
static thread_handle_t s_rx_task_handle = NULL;
static SemaphoreHandle_t s_sync_rsp_mutex = NULL;
static rpc_evt_cb_t rpc_evt_cb_table[RPC_ID__Evt_Max - RPC_ID__Evt_Base] = { NULL };
static uint32_t uid = 0;
static RingbufHandle_t s_audio_ringbuf = NULL;

/* 用于通过 uid 追踪响应信号量和回调的结构体 */
typedef struct {
	uint32_t uid;
	void * sem;
	uint16_t expected_msg_id;
	ctrl_cmd_t *response;
	bool in_use;
	bool timed_out;
} sync_rsp_t;

typedef struct {
	uint32_t uid;
	rpc_rsp_cb_t cb;
	void * timer_hdl;
	ctrl_cmd_t *app_req;  /* 保存请求指针，以便在响应或超时时释放 */
} async_rsp_t;

#define MAX_SYNC_RPC_TRANSACTIONS  5
#define MAX_ASYNC_RPC_TRANSACTIONS 5

static sync_rsp_t sync_rsp_table[MAX_SYNC_RPC_TRANSACTIONS] = { 0 };
static async_rsp_t async_rsp_table[MAX_ASYNC_RPC_TRANSACTIONS] = { 0 };

static bool lock_sync_rsp_table(TickType_t timeout_ticks)
{
	return s_sync_rsp_mutex && xSemaphoreTake(s_sync_rsp_mutex, timeout_ticks) == pdTRUE;
}

static void unlock_sync_rsp_table(void)
{
	if (s_sync_rsp_mutex) {
		xSemaphoreGive(s_sync_rsp_mutex);
	}
}

static int find_sync_slot_by_uid_locked(uint32_t resp_uid)
{
	for (int i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; ++i) {
		if (sync_rsp_table[i].in_use && sync_rsp_table[i].uid == resp_uid) {
			return i;
		}
	}
	return -1;
}

static void clear_sync_slot_locked(sync_rsp_t *slot, bool destroy_sem, bool free_response)
{
	if (!slot) {
		return;
	}

	if (free_response && slot->response) {
		HOSTED_FREE(slot->response);
	}
	if (destroy_sem && slot->sem) {
		rpcp_destroy_semaphore(slot->sem);
	}

	slot->uid = 0;
	slot->sem = NULL;
	slot->expected_msg_id = 0;
	slot->response = NULL;
	slot->in_use = false;
	slot->timed_out = false;
}

static int deliver_sync_response(ctrl_cmd_t *app_resp)
{
	void *sem = NULL;
	int result = CALLBACK_NOT_REGISTERED;

	if (!app_resp) {
		return RPC_ERR_INVALID_PARAM;
	}

	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock sync response table for uid %" PRIu32, app_resp->uid);
		return FAILURE;
	}

	int slot_index = find_sync_slot_by_uid_locked(app_resp->uid);
	if (slot_index < 0) {
		ESP_LOGW(TAG, "Drop unmatched sync resp [0x%x], uid %" PRIu32,
			 app_resp->msg_id, app_resp->uid);
		result = CALLBACK_NOT_REGISTERED;
		goto done;
	}

	sync_rsp_t *slot = &sync_rsp_table[slot_index];
	if (slot->timed_out) {
		ESP_LOGW(TAG, "Drop timed out sync resp [0x%x], uid %" PRIu32,
			 app_resp->msg_id, app_resp->uid);
		result = RET_FAIL_TIMEOUT;
		goto done;
	}

	if (slot->expected_msg_id != app_resp->msg_id) {
		ESP_LOGE(TAG, "Sync resp id mismatch: expect [0x%x], got [0x%x], uid %" PRIu32,
			 slot->expected_msg_id, app_resp->msg_id, app_resp->uid);
		result = MSG_ID_OUT_OF_ORDER;
		goto done;
	}

	if (slot->response) {
		ESP_LOGW(TAG, "Drop duplicate sync resp [0x%x], uid %" PRIu32,
			 app_resp->msg_id, app_resp->uid);
		result = FAILURE;
		goto done;
	}

	slot->response = app_resp;
	sem = slot->sem;
	app_resp = NULL;
	result = CALLBACK_AVAILABLE;

done:
	unlock_sync_rsp_table();

	if (result == CALLBACK_AVAILABLE && sem) {
		return rpcp_post_semaphore(sem);
	}
	return result;
}

/* Check and call rpc event asynchronous callback if available
 * else flag error
 *     MSG_ID_OUT_OF_ORDER - if event id is not understandable
 *     CALLBACK_NOT_REGISTERED - callback is not registered
 **/
static int call_event_callback(ctrl_cmd_t *app_event)
{
	if ((app_event->msg_id <= RPC_ID__Evt_Base) ||
	    (app_event->msg_id >= RPC_ID__Evt_Max)) {
		return MSG_ID_OUT_OF_ORDER;
	}

	if (rpc_evt_cb_table[app_event->msg_id-RPC_ID__Evt_Base]) {
		return rpc_evt_cb_table[app_event->msg_id-RPC_ID__Evt_Base](app_event);
	}

	return CALLBACK_NOT_REGISTERED;
}

/* Returns CALLBACK_AVAILABLE if a non NULL RPC event
 * callback is available. It will return failure -
 *     MSG_ID_OUT_OF_ORDER - if request msg id is unsupported
 *     CALLBACK_NOT_REGISTERED - if aync callback is not available
 **/
int is_event_callback_registered(int event)
{
	int event_cb_tbl_idx = event - RPC_ID__Evt_Base;

	if ((event<=RPC_ID__Evt_Base) || (event>=RPC_ID__Evt_Max)) {
		ESP_LOGW(TAG, "Could not identify event[%u]", event);
		return MSG_ID_OUT_OF_ORDER;
	}

	if (rpc_evt_cb_table[event_cb_tbl_idx]) {
		ESP_LOGV(TAG, "event id [0x%x]: callback %p", event, rpc_evt_cb_table[event_cb_tbl_idx]);
		return CALLBACK_AVAILABLE;
	}
	ESP_LOGD(TAG, "event id [0x%x]: No callback available", event);

	return CALLBACK_NOT_REGISTERED;
}

/* RPC TX indication */
static void rpc_tx_ind(void)
{
	ESP_LOGV(TAG, "posting rpc tx semaphore");
	rpcp_post_semaphore(s_tx_sem);
}

/* 请求 id 到响应 id 的协议映射 */
#define RPC_REQ_TO_RESP_ID(id) ((id) - RPC_ID__Req_Base + RPC_ID__Resp_Base)

/* 构造本地失败响应并通知异步回调（超时/发送失败共用）。
 * fake resp 在栈上，仅 cb 调用期间有效 */
static void notify_async_failure(ctrl_cmd_t *app_req, rpc_rsp_cb_t cb, int32_t status)
{
	ctrl_cmd_t fake_resp = { 0 };

	fake_resp.msg_type = RPC_TYPE__Resp;
	fake_resp.msg_id = RPC_REQ_TO_RESP_ID(app_req->msg_id);
	fake_resp.uid = app_req->uid;
	fake_resp.resp_event_status = status;
	fake_resp.user_ctx = app_req->user_ctx;

	if (cb) {
		cb(&fake_resp);
	}
}

/* 持锁认领异步槽位：按 uid 查表，命中则整槽拷到 out 并清零槽位。
 * 回包(RX 任务)/超时(esp_timer 任务)/发送失败(TX 任务)三方共用，
 * 谁先认领谁负责收尾（调 cb、删 timer、释放 app_req），后到者查不到直接放弃。
 * cb 调用和 free 一律在锁外执行。 */
static int claim_async_slot(uint32_t resp_uid, async_rsp_t *out)
{
	if (resp_uid == 0) {
		return FAILURE;
	}

	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock rsp table for async claim, uid %" PRIu32, resp_uid);
		return FAILURE;
	}

	for (int i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (async_rsp_table[i].uid == resp_uid) {
			*out = async_rsp_table[i];
			memset(&async_rsp_table[i], 0, sizeof(async_rsp_t));
			unlock_sync_rsp_table();
			return SUCCESS;
		}
	}

	unlock_sync_rsp_table();
	return FAILURE;
}

/* 注册异步响应槽位。app_req 所有权转移给表，由认领方释放 */
static int set_async_resp_callback(ctrl_cmd_t *app_req, void *timer_hdl)
{
	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock rsp table for async reg, req[0x%x]", app_req->msg_id);
		return CALLBACK_NOT_REGISTERED;
	}

	for (int i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (async_rsp_table[i].uid == 0) {
			async_rsp_table[i].uid = app_req->uid;
			async_rsp_table[i].cb = app_req->rpc_rsp_cb;
			async_rsp_table[i].timer_hdl = timer_hdl;
			async_rsp_table[i].app_req = app_req;
			unlock_sync_rsp_table();
			ESP_LOGD(TAG, "Register async cb for uid %" PRIu32, app_req->uid);
			return CALLBACK_SET_SUCCESS;
		}
	}

	unlock_sync_rsp_table();
	ESP_LOGE(TAG, "Async cb not registered: table full");
	return CALLBACK_NOT_REGISTERED;
}

/* 异步响应超时（esp_timer 任务上下文）。arg 传 uid 值而非 app_req 指针：
 * 回包路径可能已认领并释放了 app_req，指针会悬垂，uid 查不到则安全退出 */
static void rpc_async_timeout_handler(void *arg)
{
	uint32_t t_uid = (uint32_t)(uintptr_t)arg;
	async_rsp_t slot = { 0 };

	if (claim_async_slot(t_uid, &slot) != SUCCESS) {
		/* 回包已先到并处理，定时器句柄由回包路径删除 */
		return;
	}

	/* 认领成功的槽位 app_req/cb/timer_hdl 必然非空（注册时一次写入） */
	ESP_LOGW(TAG, "Async resp timeout for req[0x%x], uid %" PRIu32,
		 slot.app_req->msg_id, t_uid);

	notify_async_failure(slot.app_req, slot.cb, RPC_ERR_TIMEOUT);

	/* IDF 5.x 允许在定时器自身回调里删除该定时器 */
	rpcp_delete_timer(slot.timer_hdl);
	CLEANUP_APP_MSG(slot.app_req);
}

/* Set synchronous rpc response semaphore
 * In case of asynchronous request, `rx_sem` will be NULL or disregarded
 * `rpc_rsp_cb_sem_table` will be updated with NULL for async
 * In case of synchronous request, valid callback will be cached
 * This sem will posted after receiving the mapping response
 **/
static int set_sync_resp_sem(ctrl_cmd_t *app_req)
{
	int exp_resp_msg_id = RPC_REQ_TO_RESP_ID(app_req->msg_id);

	if (app_req->rx_sem)
		rpcp_destroy_semaphore(app_req->rx_sem);

	if (exp_resp_msg_id >= RPC_ID__Resp_Max) {
		ESP_LOGW(TAG, "Not able to map new request to resp id");
		return MSG_ID_OUT_OF_ORDER;
	} else if (!app_req->rpc_rsp_cb) {
		/* For sync, set sem */
		app_req->rx_sem = rpcp_create_semaphore(1);
		if (!app_req->rx_sem) {
			ESP_LOGE(TAG, "Failed to create sync semaphore for req[0x%x]", app_req->msg_id);
			return CALLBACK_NOT_REGISTERED;
		}
		rpcp_get_semaphore(app_req->rx_sem, 0);

		if (!lock_sync_rsp_table(portMAX_DELAY)) {
			ESP_LOGE(TAG, "Failed to lock sync response table for req[0x%x]", app_req->msg_id);
			rpcp_destroy_semaphore(app_req->rx_sem);
			app_req->rx_sem = NULL;
			return CALLBACK_NOT_REGISTERED;
		}

		for (int i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
			if (!sync_rsp_table[i].in_use) {
				ESP_LOGD(TAG, "Register sync sem %p for uid %ld", app_req->rx_sem, app_req->uid);
				sync_rsp_table[i].uid = app_req->uid;
				sync_rsp_table[i].sem = app_req->rx_sem;
				sync_rsp_table[i].expected_msg_id = exp_resp_msg_id;
				sync_rsp_table[i].response = NULL;
				sync_rsp_table[i].in_use = true;
				sync_rsp_table[i].timed_out = false;
				unlock_sync_rsp_table();
				return CALLBACK_SET_SUCCESS;
			}
		}
		unlock_sync_rsp_table();
		ESP_LOGE(TAG, "Symc sem not registered: out of buffer space");
		rpcp_destroy_semaphore(app_req->rx_sem);
		app_req->rx_sem = NULL;
		return CALLBACK_NOT_REGISTERED;
	} else {
		/* For async, nothing to be done */
		ESP_LOGD(TAG, "NOT Register sync sem for resp[0x%x]", exp_resp_msg_id);
		return CALLBACK_NOT_REGISTERED;
	}
}

static int cleanup_sync_async_timer_table(void)
{
	async_rsp_t pending[MAX_ASYNC_RPC_TRANSACTIONS];

	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock sync response table for cleanup");
		return FAILURE;
	}

	/* 持锁只做摘表，删 timer/free 放到锁外：esp_timer_delete 可能等待
	 * 正在执行的超时回调，而该回调会来抢这把锁，锁内删会死锁 */
	memcpy(pending, async_rsp_table, sizeof(pending));
	memset(async_rsp_table, 0, sizeof(async_rsp_table));

	for (int i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		clear_sync_slot_locked(&sync_rsp_table[i], true, true);
	}
	unlock_sync_rsp_table();

	for (int i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (pending[i].uid == 0) {
			continue;
		}
		if (pending[i].timer_hdl) {
			rpcp_delete_timer(pending[i].timer_hdl);
		}
		CLEANUP_APP_MSG(pending[i].app_req);
	}

	return SUCCESS;
}

/* 回包路径（RX 任务）：认领即处理。返回 CALLBACK_AVAILABLE 表示已按异步
 * 处理完毕（无论 cb 返回什么），CALLBACK_NOT_REGISTERED 表示未注册，
 * 上层落到同步分发。app_resp 的释放由调用方(process_rpc_rx_msg)负责 */
static int call_async_resp_callback(ctrl_cmd_t *app_resp)
{
	async_rsp_t slot = { 0 };

	// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
	if ((app_resp->msg_id < RPC_ID__Resp_Base) ||
	    (app_resp->msg_id >= RPC_ID__Resp_Max)) {
		return MSG_ID_OUT_OF_ORDER;
	}

	if (claim_async_slot(app_resp->uid, &slot) != SUCCESS) {
		return CALLBACK_NOT_REGISTERED;
	}

	/* 认领成功的槽位 app_req/cb/timer_hdl 必然非空（注册时一次写入） */
	rpcp_delete_timer(slot.timer_hdl);

	app_resp->user_ctx = slot.app_req->user_ctx;
	slot.cb(app_resp);

	CLEANUP_APP_MSG(slot.app_req);
	return CALLBACK_AVAILABLE;
}

static int wait_for_sync_response(ctrl_cmd_t *app_req)
{
	int timeout_sec = 0;
	int ret = 0;
	void *slot_sem = NULL;

	/* If timeout not specified, use default */
	if (!app_req->rsp_timeout_sec)
		timeout_sec = DEFAULT_RPC_RSP_TIMEOUT_SEC;
	else
		timeout_sec = app_req->rsp_timeout_sec;

	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock sync response table for req[0x%x]", app_req->msg_id);
		return FAILURE;
	}

	int slot_index = find_sync_slot_by_uid_locked(app_req->uid);
	if (slot_index < 0) {
		unlock_sync_rsp_table();
		ESP_LOGW(TAG, "Not able to map new request to resp id");
		return MSG_ID_OUT_OF_ORDER;
	}
	slot_sem = sync_rsp_table[slot_index].sem;
	unlock_sync_rsp_table();

	ESP_LOGV(TAG, "Wait for sync resp for Req[0x%x] with timer of %u sec",
			app_req->msg_id, timeout_sec);
	ret = rpcp_get_semaphore(slot_sem, timeout_sec*1000);
	if (!ret) {
		return RET_OK;
	}

	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock sync response table after wait for req[0x%x]", app_req->msg_id);
		return ret;
	}

	slot_index = find_sync_slot_by_uid_locked(app_req->uid);
	if (slot_index >= 0) {
		sync_rsp_t *slot = &sync_rsp_table[slot_index];
		if (slot->response) {
			unlock_sync_rsp_table();
			return RET_OK;
		}
		slot->timed_out = true;
		clear_sync_slot_locked(slot, true, false);
	}
	unlock_sync_rsp_table();
	return ret;
}

/* This function will be only invoked in synchrounous rpc response path,
 * i.e. if rpc response callbcak is not available i.e. NULL
 * This function is called after sending synchrounous rpc request to wait
 * for the response using semaphores and esp_queue
 **/
static ctrl_cmd_t * get_response(int *read_len, ctrl_cmd_t *app_req)
{
	uint8_t * buf = NULL;
	int ret = 0;

	/* Any problems in response, return NULL */
	if (!read_len || !app_req) {
		ESP_LOGE(TAG, "Invalid input parameter");
		return NULL;
	}


	/* Wait for response */
	ret = wait_for_sync_response(app_req);
	if (ret) {
		if ((ret == RET_FAIL_TIMEOUT) || (errno == ETIMEDOUT))
			ESP_LOGW(TAG, "Timeout waiting for Resp for Req[0x%x]", app_req->msg_id);
		else
			ESP_LOGE(TAG, "ERR [%u] ret[%d] for Req[0x%x]", errno, ret, app_req->msg_id);
		return NULL;
	}

	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock sync response table to consume req[0x%x]", app_req->msg_id);
		return NULL;
	}

	int slot_index = find_sync_slot_by_uid_locked(app_req->uid);
	if (slot_index < 0) {
		unlock_sync_rsp_table();
		ESP_LOGE(TAG, "Sync response slot missing for req[0x%x]", app_req->msg_id);
		return NULL;
	}

	sync_rsp_t *slot = &sync_rsp_table[slot_index];
	if (!slot->response) {
		unlock_sync_rsp_table();
		ESP_LOGE(TAG, "Sync response missing for req[0x%x]", app_req->msg_id);
		return NULL;
	}

	buf = (uint8_t *)slot->response;
	*read_len = sizeof(ctrl_cmd_t);
	slot->response = NULL;
	clear_sync_slot_locked(slot, true, false);
	unlock_sync_rsp_table();
	return (ctrl_cmd_t*)buf;

	return NULL;
}

ctrl_cmd_t * rpc_wait_and_parse_sync_resp(ctrl_cmd_t *app_req)
{
	ctrl_cmd_t * rx_buf = NULL;
	int rx_buf_len = 0;

	rx_buf = get_response(&rx_buf_len, app_req);
	if (!rx_buf || !rx_buf_len) {
		ESP_LOGE(TAG, "Response not received for [0x%x]", app_req->msg_id);
		if (rx_buf) {
			HOSTED_FREE(rx_buf);
		}
	}
	HOSTED_FREE(app_req);
	return rx_buf;
}

/**
 * 使用 RPC 请求 API 时的入口函数
 * 此函数将 RPC 请求编码为 protobuf 格式并发送到 ESP32
 * 它会将应用层结构体 `ctrl_cmd_t` 复制到
 * protobuf RPC 请求 `Rpc` 中
 */
int rpc_send_req(ctrl_cmd_t *app_req)
{
	if (!app_req) {
		ESP_LOGE(TAG, "Invalid param in rpc_send_req");
		return FAILURE;
	}


	if (!lock_sync_rsp_table(portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to lock sync response table for uid allocation");
		return FAILURE;
	}
	uid++;
	// 处理 uid 溢出情况
	if (!uid)
		uid++;
	app_req->uid = uid;
	unlock_sync_rsp_table();

	ESP_LOGD(TAG, "app_req msgid[0x%x] with uid %" PRIu32, app_req->msg_id, app_req->uid);
	if (!app_req->rpc_rsp_cb) {
		/* 仅用于同步处理 */
		if (set_sync_resp_sem(app_req)) {
			ESP_LOGE(TAG, "could not set sync resp sem for req[0x%x]",app_req->msg_id);
			goto fail_req;
		}
	}

	app_req->msg_type = RPC_TYPE__Req;

	ESP_LOGV(TAG, "queueing rpc tx q with uid %" PRIu32, app_req->uid);
	if (rpcp_queue_item(s_tx_queue, &app_req, HOSTED_BLOCK_MAX)) {
	  ESP_LOGE(TAG, "Failed to new app rpc req[0x%x] in tx queue", app_req->msg_id);
	  goto fail_req;
	}

	rpc_tx_ind();

	/* TODO : 已注释，需再次审查以避免重复释放 */
	//H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);

	return SUCCESS;

fail_req:
	if (app_req->rx_sem)
		rpcp_destroy_semaphore(app_req->rx_sem);

	H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);
	HOSTED_FREE(app_req);

	return FAILURE;
}

// 处理 RPC 发送消息
static int process_rpc_tx_msg(ctrl_cmd_t *app_req)
{
	Rpc req = {0};
	uint32_t tx_len = 0;
	uint8_t *tx_data = NULL;
	// int ret = SUCCESS;
	int32_t failure_status = 0;
	bool async_registered = false;

	req.msg_type = RPC_TYPE__Req;
	rpc__init(&req);

	req.msg_id = (RpcId)app_req->msg_id;
	req.uid = app_req->uid;
	ESP_LOGD(TAG, "<-- RPC_Req [0x%x], uid %ld", app_req->msg_id, app_req->uid);
	/* payload case is exact match to msg_id */
	req.payload_case = (Rpc__PayloadCase)app_req->msg_id;

	// TODO: 根据 msg_id 将 app_req 中的数据复制到对应的 req.payload_union 中
	if (compose_rpc_req(&req, app_req, &failure_status)) {
		ESP_LOGE(TAG, "compose_rpc_req failed for [0x%x]", app_req->msg_id);
		goto fail_req;
	}

	/* 序列化消息 */
	tx_len = rpc__get_packed_size(&req);
	if (!tx_len) {
		ESP_LOGE(TAG, "Invalid tx length");
		failure_status = RPC_ERR_ENCODE_FAILED;
		goto fail_req;
	}

	HOSTED_CALLOC(uint8_t, tx_data, tx_len, fail_req0);

	/* 5. 异步：注册槽位 -> 启动超时定时器 -> 发送。
	 * 先注册保证回包到达时槽位一定在；注册后的任何失败都走 fail_req，
	 * 由失败路径按 async_registered 重新认领槽位收尾 */
	if (app_req->rpc_rsp_cb) {
		int timeout_sec = app_req->rsp_timeout_sec ?
				app_req->rsp_timeout_sec : DEFAULT_RPC_RSP_TIMEOUT_SEC;

		void *timer_hdl = rpcp_create_timer(rpc_async_timeout_handler,
				(void *)(uintptr_t)app_req->uid);
		if (!timer_hdl) {
			ESP_LOGE(TAG, "Failed to create async resp timer for req[0x%x]", req.msg_id);
			failure_status = RPC_ERR_NO_MEMORY;
			goto fail_req;
		}

		if (set_async_resp_callback(app_req, timer_hdl) != CALLBACK_SET_SUCCESS) {
			ESP_LOGE(TAG, "could not set async cb for req[0x%x]", req.msg_id);
			rpcp_delete_timer(timer_hdl);
			failure_status = RPC_ERR_TABLE_FULL;
			goto fail_req;
		}
		async_registered = true;

		if (rpcp_start_timer(timer_hdl, (uint32_t)timeout_sec * 1000U)) {
			ESP_LOGE(TAG, "Failed to start async resp timer for req[0x%x]", req.msg_id);
			failure_status = RPC_ERR_FAILURE;
			goto fail_req;
		}
	}

	rpc__pack(&req, tx_data);
	ESP_LOGD(TAG, "sending rpc req[0x%x]", req.msg_id);
	if (serial_transfer_send_packed(PACK_TYPE_RPC, tx_data, tx_len) < 0) {
		ESP_LOGE(TAG, "Send RPC req[0x%x] failed", req.msg_id);
		failure_status = RPC_ERR_TX_FAILED;
		goto fail_req;
	}

	ESP_LOGD(TAG, "Sent RPC_Req[0x%x]", req.msg_id);

	/* 8. Free hook for application */
	H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);
	/* 释放资源 */
	/* 9. Cleanup */
	HOSTED_FREE(tx_data);
	RPC_FREE_BUFFS();
	return SUCCESS;

fail_req0:
	failure_status = RPC_ERR_NO_MEMORY;
fail_req:
	ESP_LOGW(TAG, "fail1");
	if (app_req->rpc_rsp_cb) {
		/* 异步失败：先取回槽位所有权再通知。
		 * 若已注册但认领失败，说明超时回调抢先收尾（含释放 app_req），
		 * 这里不得再碰 app_req、不得重复通知 */
		bool owned = true;
		if (async_registered) {
			async_rsp_t slot = { 0 };
			owned = (claim_async_slot(app_req->uid, &slot) == SUCCESS);
			if (owned && slot.timer_hdl) {
				rpcp_delete_timer(slot.timer_hdl);
			}
		}

		if (owned) {
			notify_async_failure(app_req, app_req->rpc_rsp_cb, failure_status);
		}

		/* 13. Cleanup */
		HOSTED_FREE(tx_data);
		if (owned) {
			RPC_FREE_BUFFS();
			CLEANUP_APP_MSG(app_req);
		}
		return FAILURE;
	} else {
		// 将失败响应放入接收队列，防止死等超时
		ESP_LOGW(TAG, "RPC Sync proc failed");

		ctrl_cmd_t *app_resp = NULL;

		HOSTED_CALLOC(ctrl_cmd_t, app_resp, sizeof(ctrl_cmd_t), fail_req2);

		app_resp->msg_type = RPC_TYPE__Resp;
		app_resp->msg_id = RPC_REQ_TO_RESP_ID(app_req->msg_id);
		app_resp->uid = app_req->uid;
		app_resp->resp_event_status = failure_status;

		if (deliver_sync_response(app_resp) < 0) {
			ESP_LOGE(TAG, "Failed to deliver local sync error for uid %ld", app_resp->uid);
			HOSTED_FREE(app_resp);
		}
	}

fail_req2:
	ESP_LOGW(TAG, "fail2");
	/* 13. Cleanup（同步失败路径）。app_req 不能在此释放：
	 * 调用方正在等待，将在 rpc_wait_and_parse_sync_resp() 中释放 */
	H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);

	HOSTED_FREE(tx_data);
	RPC_FREE_BUFFS();

	return FAILURE;
}

/* Process RPC msg (response or event) received from ESP32 */
static int process_rpc_rx_msg(Rpc * proto_msg)
{
	ctrl_cmd_t *app_resp = NULL;
	ctrl_cmd_t *app_event = NULL;

	/* 1. Check if valid proto msg */
	if (!proto_msg) {
		return FAILURE;
	}

	/* Note: free proto_msg at the end of processing*/

	/* 2. Check if it is event msg */
	if (proto_msg->msg_type == RPC_TYPE__Event) {
		/* Events are handled only asynchronously */
		ESP_LOGD(TAG, "Received Event [0x%x]", proto_msg->msg_id);
		/* check if callback is available.
		 * if not, silently drop the msg */
		if (CALLBACK_AVAILABLE ==
			is_event_callback_registered(proto_msg->msg_id)) {
			/* if event callback is registered, we need to
			 * parse the event into app structs and
			 * call the registered callback function
			 **/

			/* Allocate app struct for event */

			HOSTED_CALLOC(ctrl_cmd_t, app_event, sizeof(ctrl_cmd_t), free_buffers);

			/* Decode protobuf buffer of event and
			 * copy into app structures */
			if (rpc_parse_evt(proto_msg, app_event)) {
				ESP_LOGE(TAG, "failed to parse event");
				goto free_buffers;
			}

			/* callback to registered function.
			 * 注意所有权：事件回调（rpc_wrap.c rpc_event_callback）
			 * 自己 CLEANUP_RPC 释放 app_event，这里不再释放 */
			call_event_callback(app_event);
			app_event = NULL;
		} else
			goto free_buffers;

	/* 3. Check if it is response msg */
	} else if (proto_msg->msg_type == RPC_TYPE__Resp) {
		ESP_LOGD(TAG, "Received Resp [0x%x]", proto_msg->msg_id);

		/* Allocate app struct for response */
		HOSTED_CALLOC(ctrl_cmd_t, app_resp, sizeof(ctrl_cmd_t), free_buffers);

		/* Decode protobuf buffer of response and
		 * copy into app structures */
		if (rpc_parse_rsp(proto_msg, app_resp)) {
			ESP_LOGE(TAG, "failed to parse response, [0x%x]", proto_msg->msg_id);
			goto free_buffers;
		}

		/* 先尝试异步认领（内部持锁，一次完成判断+认领，无 check-then-act
		 * 竞态）；未注册则按同步响应分发 */
		if (CALLBACK_AVAILABLE == call_async_resp_callback(app_resp)) {
			/* cb 已返回，按契约由框架释放 resp 及其附属缓冲
			 * （如 sd_music_list 的 files 数组挂在 app_free_buff_hdl） */
			CLEANUP_APP_MSG(app_resp);
			app_resp = NULL;
		} else {
			if (deliver_sync_response(app_resp) != RET_OK) {
				goto free_buffers;
			}
			app_resp = NULL;
		}

	} else {
		/* 4. some unsupported msg, drop it */
		ESP_LOGE(TAG, "Incorrect RPC Msg Type[%u]",proto_msg->msg_type);
		goto free_buffers;
	}
	rpc__free_unpacked(proto_msg, NULL);
	proto_msg = NULL;
	return SUCCESS;

	/* 5. cleanup */
free_buffers:
	rpc__free_unpacked(proto_msg, NULL);
	proto_msg = NULL;
	/* 用 CLEANUP_APP_MSG 而非裸 free：解析层可能已把附属缓冲
	 * （如列表数组）挂在 app_free_buff_hdl 上 */
	CLEANUP_APP_MSG(app_event);
	CLEANUP_APP_MSG(app_resp);
	return RPC_ERR_DECODE_FAILED;
}

// 发送任务
static void rpc_tx_task(void *arg)
{
    ctrl_cmd_t *app_req;

    while (1) {
        // 等待信号量通知
        rpcp_get_semaphore(s_tx_sem, HOSTED_BLOCKING);
		ESP_LOGV(TAG, "RPC TX semaphore acquired");

		ESP_LOGV(TAG, "Dequeueing RPC TX Q");
		if (rpcp_dequeue_item(s_tx_queue, &app_req, HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG, "RPC TX Q Failed to dequeue");
			continue;
		}

		if (app_req) {
			ESP_LOGV(TAG, "Processing RPC TX msg");
			process_rpc_tx_msg(app_req);
			/* app_req lifecycle:
			 * - Async requests: Stored in async_rsp_table, freed when response arrives or timeout
			 * - Sync requests: Freed by caller in rpc_wait_and_parse_sync_resp() after response
			 * - Failed requests: Freed in process_rpc_tx_msg() failure path
			 * So we don't free anything here. */
		} else {
			ESP_LOGE(TAG, "RPC Tx Q empty or uninitialised");
			continue;
		}
    }

    vTaskDelete(NULL);
}


// 传输层接收回调
static void transfer_recv_cb(uint8_t type, const uint8_t *data, size_t len, void *user_ctx)
{
    ESP_LOGD(TAG, "Received packet: type=%d, len=%d", type, len);

	if (type == PACK_TYPE_RPC) {
		/* 1. Decode protobuf */
		Rpc *resp = rpc__unpack(NULL, len, data);
		if (!resp) {
			ESP_LOGE(TAG, "Failed to unpack RPC message");
			return;
		}

		/* 2. Process RPC message */
		process_rpc_rx_msg(resp);

	} else if (type == PACK_TYPE_AUDIO_OPUS)
	{
		int ret = audio_opus_frame_write(data, len);
	} else {
		ESP_LOGW(TAG, "Unknown packet type received: %d", type);
	}

}

/* Set rpc event callback
 * `rpc_evt_cb_table` will be updated with NULL by default
 * when user sets event callback, user provided function pointer
 * will be registered with user function
 * If user does not register event callback,
 * events received from ESP32 will be dropped
 **/
int set_event_callback(int event, rpc_rsp_cb_t event_cb)
{
	int event_cb_tbl_idx = event - RPC_ID__Evt_Base;

	if ((event<=RPC_ID__Evt_Base) || (event>=RPC_ID__Evt_Max)) {
		ESP_LOGW(TAG, "Could not identify event[0x%x]", event);
		return MSG_ID_OUT_OF_ORDER;
	}
	rpc_evt_cb_table[event_cb_tbl_idx] = event_cb;
	return CALLBACK_SET_SUCCESS;
}

/* Assign NULL event callback */
int reset_event_callback(int event)
{
	return set_event_callback(event, NULL);
}


int rpc_701_init(const rpc_701_config_t *config)
{
    if (!config || !config->write_cb) {
        ESP_LOGE(TAG, "Invalid config");
        return FAILURE;
    }

	s_sync_rsp_mutex = xSemaphoreCreateMutex();
	if (!s_sync_rsp_mutex) {
		ESP_LOGE(TAG, "Failed to create sync response mutex");
		return FAILURE;
	}

    // 创建计数信号量
    s_tx_sem = rpcp_create_semaphore(MAX_SYNC_RPC_TRANSACTIONS +
			MAX_ASYNC_RPC_TRANSACTIONS);
    if (!s_tx_sem) {
        ESP_LOGE(TAG, "Failed to create TX semaphore");
        s_tx_queue = NULL;
        return FAILURE;
    }
	rpcp_get_semaphore(s_tx_sem, 0);
	
	s_rx_queue = rpcp_create_queue(RPC_RX_QUEUE_SIZE, sizeof(esp_queue_elem_t));


    // 创建发送队列
    s_tx_queue = rpcp_create_queue(RPC_TX_QUEUE_SIZE, sizeof(void *));
    if (!s_tx_queue || !s_rx_queue) {
        ESP_LOGE(TAG, "Failed to create TX or RX queue");
        goto free_bufs;
    }

	// 创建发送任务
	if (xTaskCreate(rpc_tx_task, "rpc_tx", RPC_TASK_STACK_SIZE, NULL, RPC_TASK_PRIORITY, &s_tx_task_handle) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create RPC TX task");
		goto free_bufs;
	}
	
	// if (xTaskCreate(rpc_rx_task, "rpc_rx", RPC_TASK_STACK_SIZE, NULL, RPC_TASK_PRIORITY, &s_rx_task_handle) != pdPASS) {
	// 	ESP_LOGE(TAG, "Failed to create RPC RX task");
	// 	goto free_bufs;
	// }

    // 初始化传输层
    serial_transfer_init(config->write_cb, transfer_recv_cb, NULL);

    // 初始化音频帧缓冲区
    s_audio_ringbuf = xRingbufferCreate(AUDIO_FRAME_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!s_audio_ringbuf) {
        ESP_LOGE(TAG, "Failed to initialize audio frame buffer");
    } else {
        ESP_LOGI(TAG, "Audio frame buffer initialized: %d bytes", AUDIO_FRAME_BUF_SIZE);
    }

    ESP_LOGI(TAG, "RPC_701 initialized");
	return SUCCESS;
free_bufs:
	rpc_701_deinit();
	return FAILURE;
}

/* cancel thread for rpc RX path handling */
static int cancel_rpc_threads(void)
{
	// int ret1 = 0, ret2 =0;

	if (s_tx_task_handle)
		vTaskDelete(s_tx_task_handle);

	if (s_rx_task_handle)
		vTaskDelete(s_rx_task_handle);

	// if (ret1 || ret2) {
	// 	ESP_LOGE(TAG, "pthread_cancel rpc threads failed");
	// 	return FAILURE;
	// }

	return SUCCESS;
}

void rpc_701_feed_rx(uint8_t *data, size_t len)
{
    serial_transfer_process(data, len);
}

void rpc_701_reset_rx(void)
{
    serial_transfer_reset();
}

void rpc_701_deinit(void)
{
	
    ESP_LOGI(TAG, "RPC_701 deinitialized");

    /* 销毁前清空队列 */
	if (s_rx_queue) {
		esp_queue_elem_t elem;
		/* Flush all remaining items and free their buffers */
		while (rpcp_dequeue_item(s_rx_queue, &elem, 0) == 0) {
			if (elem.buf) {
				HOSTED_FREE(elem.buf);
			}
		}
		rpcp_destroy_queue(s_rx_queue);
		s_rx_queue = NULL;
	}
	if (s_tx_queue) {
		void *buf_ptr;
		/* Flush all remaining items and free their buffers */
		while (rpcp_dequeue_item(s_tx_queue, &buf_ptr, 0) == 0) {
			if (buf_ptr) {
				HOSTED_FREE(buf_ptr);
			}
		}
		rpcp_destroy_queue(s_tx_queue);
		s_tx_queue = NULL;
	}

	if (s_tx_sem) {
		if (rpcp_destroy_semaphore(s_tx_sem)) {
			// ret = FAILURE;
			ESP_LOGE(TAG, "read sem tx deinit failed");
		}
		s_tx_sem = NULL;
	}

	cleanup_sync_async_timer_table();
	if (s_sync_rsp_mutex) {
		vSemaphoreDelete(s_sync_rsp_mutex);
		s_sync_rsp_mutex = NULL;
	}

	cancel_rpc_threads();

	// 清理音频帧缓冲区
	if (s_audio_ringbuf) {
		vRingbufferDelete(s_audio_ringbuf);
		s_audio_ringbuf = NULL;
	}

	serial_transfer_deinit();
}

/* ============================================================
 * Audio OPUS Frame Buffer Implementation
 * ============================================================ */

int audio_opus_frame_write(const uint8_t *data, size_t len)
{
	if (!s_audio_ringbuf || !data) {
		ESP_LOGE(TAG, "Invalid parameters for audio_opus_frame_write");
		return -1;
	}

	if (len == 0) {
		ESP_LOGW(TAG, "Ignoring zero-length frame");
		return 0;
	}

	// Try to write, dropping old frames if buffer is full
	int attempts = 0;
	const int max_attempts = 10;

	while (xRingbufferSend(s_audio_ringbuf, data, len, 0) != pdTRUE) {
		size_t recv_len;
		uint8_t *old_frame = xRingbufferReceive(s_audio_ringbuf, &recv_len, 0);
		if (!old_frame) {
			ESP_LOGE(TAG, "Buffer full and cannot free space");
			return -1;
		}
		vRingbufferReturnItem(s_audio_ringbuf, old_frame);

		if (++attempts > max_attempts) {
			ESP_LOGE(TAG, "Cannot write frame after %d drops", attempts);
			return -1;
		}
		ESP_LOGD(TAG, "Dropped old frame (%zu bytes), attempt %d", recv_len, attempts);
	}

	ESP_LOGV(TAG, "Wrote OPUS frame: %zu bytes", len);
	return len;
}

int audio_opus_frame_read(uint8_t *data, size_t max_len)
{
	if (!s_audio_ringbuf || !data) {
		ESP_LOGE(TAG, "Invalid parameters for audio_opus_frame_read");
		return -1;
	}

	size_t recv_len;
	uint8_t *frame = NULL;
	int64_t start_time = esp_timer_get_time();
	const int64_t timeout_us = 70000;  // 60ms超时

	while (1) {
		frame = xRingbufferReceive(s_audio_ringbuf, &recv_len, 0);
		if (frame) {
			break;
		}
		if (esp_timer_get_time() - start_time >= timeout_us) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
	}

	if (!frame) {
		return 0;
	}

	if (recv_len > max_len) {
		ESP_LOGW(TAG, "Frame size (%zu) exceeds max_len (%zu), truncating", recv_len, max_len);
		recv_len = max_len;
	}

	memcpy(data, frame, recv_len);
	vRingbufferReturnItem(s_audio_ringbuf, frame);

	return recv_len;
}

size_t audio_opus_frame_available(void)
{
	if (!s_audio_ringbuf) {
		return 0;
	}
	return xRingbufferGetCurFreeSize(s_audio_ringbuf);
}

void audio_opus_frame_flush(void)
{
	if (!s_audio_ringbuf) {
		return;
	}
	size_t recv_len;
	uint8_t *frame;
	while ((frame = xRingbufferReceive(s_audio_ringbuf, &recv_len, 0)) != NULL) {
		vRingbufferReturnItem(s_audio_ringbuf, frame);
	}
}
