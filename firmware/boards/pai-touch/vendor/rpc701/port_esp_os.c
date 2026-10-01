#include "port_esp_os.h"
#include "esp_log.h"

static const char* TAG = "port_os";

ESP_EVENT_DEFINE_BASE(RPC_VB_EVENT);


int rpcp_event_post(esp_event_base_t event_base, int32_t event_id,
		void* event_data, size_t event_data_size, uint32_t ticks_to_wait)
{
	ESP_LOGV(TAG, "base %s, event %ld recvd --> event_data:%p event_data_size: %u",event_base,event_id, event_data, event_data_size);
	return esp_event_post(event_base, event_id, event_data, event_data_size, ticks_to_wait);
}

void *rpcp_create_semaphore(int maxCount)
{
	semaphore_handle_t *sem_id = NULL;

	sem_id = (semaphore_handle_t*)malloc(sizeof(semaphore_handle_t));
	if (!sem_id) {
		ESP_LOGE(TAG, "Sem allocation failed\n");
		return NULL;
	}

	if (maxCount > 1)
		*sem_id = xSemaphoreCreateCounting(maxCount, 0);
	else
		*sem_id = xSemaphoreCreateBinary();

	if (!*sem_id) {
		ESP_LOGE(TAG, "sem create failed\n");
		free(sem_id);
		return NULL;
	}

	xSemaphoreGive(*sem_id);

	return sem_id;
}

int rpcp_post_semaphore(void *semaphore_handle)
{
	semaphore_handle_t *sem_id = NULL;
	int sem_posted = 0;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 3\n");
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;
	sem_posted = xSemaphoreGive(*sem_id);
	if (pdTRUE == sem_posted)
		return RET_OK;

	return RET_FAIL;
}

int rpcp_post_semaphore_from_isr(void *semaphore_handle)
{
	semaphore_handle_t *sem_id = NULL;
	int sem_posted = 0;
	BaseType_t mustYield = false;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 3\n");
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;

	sem_posted = xSemaphoreGiveFromISR(*sem_id, &mustYield);
	if (mustYield) {
#if defined(__cplusplus) && (__cplusplus >  201703L)
		portYIELD_FROM_ISR(mustYield);
#else
		portYIELD_FROM_ISR();
#endif
	}
	if (pdTRUE == sem_posted)
		return RET_OK;

	return RET_FAIL;
}

int rpcp_get_semaphore(void *semaphore_handle, int timeout)
{
	semaphore_handle_t *sem_id = NULL;
	int sem_acquired = 0;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 1\n\r");
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;
	if (!*sem_id) {
		ESP_LOGE(TAG, "Uninitialized sem id 2\n\r");
		return RET_INVALID;
	}

	if (!timeout) {
		/* non blocking */
		sem_acquired = xSemaphoreTake(*sem_id, 0);
	} else if (timeout < 0) {
		/* Blocking */
		sem_acquired = xSemaphoreTake(*sem_id, portMAX_DELAY);
	} else {
		sem_acquired = xSemaphoreTake(*sem_id, pdMS_TO_TICKS(timeout));
	}

	if (sem_acquired == pdTRUE)
		return RET_OK;

	return RET_FAIL_TIMEOUT;
}

int rpcp_destroy_semaphore(void *semaphore_handle)
{
	int ret = RET_OK;
	semaphore_handle_t *sem_id = NULL;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 4\n");
		assert(semaphore_handle);
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;

	vSemaphoreDelete(*sem_id);

	free(semaphore_handle);

	return ret;
}

/* -------- Queue --------------- */

int rpcp_queue_item(void *queue_handle, void *item, int timeout)
{
	queue_handle_t *q_id = NULL;
	int item_added_in_back = 0;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 3\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;
	item_added_in_back = xQueueSendToBack(*q_id, item, timeout);
	if (pdTRUE == item_added_in_back)
		return RET_OK;

	return RET_FAIL;
}

void *rpcp_create_queue(uint32_t qnum_elem, uint32_t qitem_size)
{
	queue_handle_t *q_id = NULL;

	q_id = (queue_handle_t*)malloc(sizeof(queue_handle_t));
	if (!q_id) {
		ESP_LOGE(TAG, "Q allocation failed\n");
		return NULL;
	}

	*q_id = xQueueCreate(qnum_elem, qitem_size);
	if (!*q_id) {
		ESP_LOGE(TAG, "Q create failed\n");
		return NULL;
	}

	return q_id;
}

int rpcp_dequeue_item(void *queue_handle, void *item, int timeout)
{
	queue_handle_t *q_id = NULL;
	int item_retrieved = 0;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized Q id 1\n\r");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;
	if (!*q_id) {
		ESP_LOGE(TAG, "Uninitialized Q id 2\n\r");
		return RET_INVALID;
	}

	if (!timeout) {
		/* non blocking */
		item_retrieved = xQueueReceive(*q_id, item, 0);
	} else if (timeout < 0) {
		/* Blocking */
		item_retrieved = xQueueReceive(*q_id, item, portMAX_DELAY);
	} else {
		item_retrieved = xQueueReceive(*q_id, item, pdMS_TO_TICKS(timeout));
	}

	if (item_retrieved == pdTRUE)
		return RET_OK;

	return RET_FAIL;
}

int rpcp_queue_msg_waiting(void *queue_handle)
{
	queue_handle_t *q_id = NULL;
	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 9\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;
	return uxQueueMessagesWaiting(*q_id);
}

int rpcp_destroy_queue(void *queue_handle)
{
	int ret = RET_OK;
	queue_handle_t *q_id = NULL;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized Q id 4\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;

	vQueueDelete(*q_id);

	free(queue_handle);

	return ret;
}

int rpcp_reset_queue(void *queue_handle)
{
	queue_handle_t *q_id = NULL;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized Q id 5\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;

	return xQueueReset(*q_id);
}

/* -------- Timer --------------- */
/* esp_timer_handle_t 本身就是不透明指针，直接作为 void* 句柄透传 */

void *rpcp_create_timer(rpc_timer_cb_t callback, void *arg)
{
	esp_timer_handle_t handle = NULL;

	if (!callback) {
		ESP_LOGE(TAG, "Timer callback is NULL\n");
		return NULL;
	}

	esp_timer_create_args_t args = {
		.callback = callback,
		.arg = arg,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "rpcp_timer",
	};
	if (esp_timer_create(&args, &handle) != ESP_OK) {
		ESP_LOGE(TAG, "Timer create failed\n");
		return NULL;
	}

	return handle;
}

int rpcp_start_timer(void *timer, uint32_t timeout_ms)
{
	if (!timer) {
		ESP_LOGE(TAG, "%s: NULL timer\n", __func__);
		return RET_INVALID;
	}

	if (esp_timer_start_once((esp_timer_handle_t)timer, (uint64_t)timeout_ms * 1000ULL) != ESP_OK) {
		return RET_FAIL;
	}
	return RET_OK;
}

int rpcp_stop_timer(void *timer)
{
	if (!timer) {
		ESP_LOGE(TAG, "%s: NULL timer\n", __func__);
		return RET_INVALID;
	}

	/* 未在运行返回 ESP_ERR_INVALID_STATE，视为已停止，幂等 */
	esp_timer_stop((esp_timer_handle_t)timer);
	return RET_OK;
}

int rpcp_delete_timer(void *timer)
{
	if (!timer) {
		ESP_LOGE(TAG, "%s: NULL timer\n", __func__);
		return RET_INVALID;
	}

	esp_timer_stop((esp_timer_handle_t)timer);
	/* IDF 5.x 允许在定时器自身回调里 delete 自己（超时路径依赖这一点） */
	if (esp_timer_delete((esp_timer_handle_t)timer) != ESP_OK) {
		ESP_LOGE(TAG, "Timer delete failed\n");
		return RET_FAIL;
	}
	return RET_OK;
}