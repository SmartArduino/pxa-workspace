#pragma once
#include <stddef.h>
#include <stdint.h>
#include <cassert>
union StaticQueue_t { max_align_t alignment; unsigned char bytes[256]; };
struct StaticSemaphore_t { bool locked; };
struct StaticTask_t { unsigned char bytes[256]; };
typedef uint32_t StackType_t;
#define configASSERT(condition) assert(condition)
typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
typedef uint32_t TickType_t;
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
void* heap_caps_malloc(size_t, unsigned);
void* heap_caps_calloc(size_t, size_t, unsigned);
QueueHandle_t xQueueCreate(unsigned, size_t);
QueueHandle_t xQueueCreateWithCaps(unsigned, size_t, unsigned);
QueueHandle_t xQueueCreateStatic(unsigned, size_t, uint8_t*, StaticQueue_t*);
int xQueueSend(QueueHandle_t, const void*, uint32_t);
int xQueueOverwrite(QueueHandle_t, const void*);
int xQueueReceive(QueueHandle_t, void*, uint32_t);
void xQueueReset(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void(*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*);
TaskHandle_t xTaskCreateStatic(void(*)(void*), const char*, uint32_t, void*, unsigned, StackType_t*, StaticTask_t*);
void vTaskDelete(TaskHandle_t);
TickType_t xTaskGetTickCount();
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(int, TickType_t);
void vTaskDelayUntil(TickType_t*, TickType_t);
void vTaskDelay(TickType_t);
unsigned uxTaskGetStackHighWaterMark(void*);

typedef int esp_audio_err_t;
typedef void* esp_audio_simple_dec_handle_t;
enum {ESP_AUDIO_ERR_OK=0, ESP_AUDIO_ERR_BUFF_NOT_ENOUGH=-8, ESP_AUDIO_ERR_MEM_LACK=-2, ESP_AUDIO_ERR_NOT_SUPPORT=-7, ESP_AUDIO_SIMPLE_DEC_TYPE_OGG=2};
struct esp_audio_simple_dec_cfg_t { int dec_type; };
struct esp_audio_simple_dec_raw_t { uint8_t* buffer; size_t len, consumed; bool eos; };
struct esp_audio_simple_dec_out_t { uint8_t* buffer; size_t len, needed_size, decoded_size; };
struct esp_audio_simple_dec_info_t { int bits_per_sample; uint32_t sample_rate; int channel; };
int esp_vorbis_dec_register();
int esp_opus_dec_register();
int esp_ogg_dec_register();
enum { ESP_AUDIO_TYPE_VORBIS, ESP_AUDIO_TYPE_OPUS };
void esp_audio_dec_unregister(int);
int esp_ogg_dec_unregister();
int esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*, esp_audio_simple_dec_handle_t*);
int esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t);
int esp_audio_simple_dec_reset(esp_audio_simple_dec_handle_t);
int esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t, esp_audio_simple_dec_raw_t*, esp_audio_simple_dec_out_t*);
int esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t, esp_audio_simple_dec_info_t*);
