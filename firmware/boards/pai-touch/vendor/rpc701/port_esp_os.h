#ifndef __PORT_ESP_HOSTED_HOST_OS_H
#define __PORT_ESP_HOSTED_HOST_OS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_event.h"

#include "esp_task.h"
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

#include "esp_timer.h"
// #include "esp_event.h"
// #include "esp_heap_caps.h"
// #include "esp_netif_types.h"
// #include "esp_wifi_types.h"
// #include "esp_wifi_default.h"

#define HOSTED_BLOCKING                              -1
#define HOSTED_NON_BLOCKING                          0

#define thread_handle_t                              TaskHandle_t
#define queue_handle_t                               QueueHandle_t
#define semaphore_handle_t                           SemaphoreHandle_t
#define mutex_handle_t                               SemaphoreHandle_t

#define RET_OK                                       0
#define RET_FAIL                                     -1
#define RET_INVALID                                  -2
#define RET_FAIL_MEM                                 -3
#define RET_FAIL4                                    -4
#define RET_FAIL_TIMEOUT                             -5

#define HOSTED_FREE                                  free
#define HOSTED_CALLOC(struct_name, buff, nbytes, gotosym) do {    \
	buff = (struct_name *)calloc(1, nbytes);	  \
	if (!buff) {                                                  \
		ESP_LOGE(TAG, "%s, Failed to allocate memory", __func__);     \
		goto gotosym;                                             \
	}                                                             \
} while(0);

#define HOSTED_MALLOC(struct_name, buff, nbytes, gotosym) do {    \
	buff = (struct_name *)malloc(nbytes);		  \
	if (!buff) {                                                  \
		ESP_LOGE(TAG, "%s, Failed to allocate memory", __func__);     \
		goto gotosym;                                             \
	}                                                             \
} while(0);

#define HOSTED_BLOCK_MAX                             portMAX_DELAY

int rpcp_event_post(esp_event_base_t event_base, int32_t event_id,
		void* event_data, size_t event_data_size, uint32_t ticks_to_wait);

void *rpcp_create_semaphore(int maxCount);
int rpcp_post_semaphore(void *semaphore_handle);
int rpcp_post_semaphore_from_isr(void *semaphore_handle);
int rpcp_get_semaphore(void *semaphore_handle, int timeout);
int rpcp_destroy_semaphore(void *semaphore_handle);

void *rpcp_create_queue(uint32_t qnum_elem, uint32_t qitem_size);
int rpcp_queue_item(void *queue_handle, void *item, int timeout);
int rpcp_dequeue_item(void *queue_handle, void *item, int timeout);
int rpcp_queue_msg_waiting(void *queue_handle);
int rpcp_destroy_queue(void *queue_handle);
int rpcp_reset_queue(void *queue_handle);

/* Timer interfaces */
typedef void (*rpc_timer_cb_t)(void *arg);

void *rpcp_create_timer(rpc_timer_cb_t callback, void *arg);
int rpcp_start_timer(void *timer, uint32_t timeout_ms);
int rpcp_stop_timer(void *timer);
int rpcp_delete_timer(void *timer);

#endif
