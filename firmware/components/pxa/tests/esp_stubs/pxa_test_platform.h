#ifndef PXA_TEST_PLATFORM_H
#define PXA_TEST_PLATFORM_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef int esp_err_t;
typedef int portMUX_TYPE;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef int esp_http_client_method_t;
typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;
typedef void *esp_http_client_handle_t;
typedef struct {
    void *user_data;
    int event_id;
    char *header_key;
    char *header_value;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    int method, timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    void (*crt_bundle_attach)(void);
} esp_http_client_config_t;

enum { ESP_OK, ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT };
enum { HTTP_METHOD_GET, HTTP_METHOD_HEAD, HTTP_METHOD_POST, HTTP_METHOD_PUT,
       HTTP_METHOD_PATCH, HTTP_METHOD_DELETE };
enum { HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA };
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define portENTER_CRITICAL(lock) ((void)(lock), test_enter())
#define portEXIT_CRITICAL(lock) ((void)(lock), test_leave())
#define taskENTER_CRITICAL(lock) portENTER_CRITICAL(lock)
#define taskEXIT_CRITICAL(lock) portEXIT_CRITICAL(lock)

void test_log(const char *, const char *, ...);
static inline void esp_log_write(esp_log_level_t level, const char *tag,
                                 const char *format, ...) {
    (void)level;
    (void)tag;
    (void)format;
}
void test_enter(void);
void test_leave(void);
void *heap_caps_malloc(size_t, unsigned);
void *heap_caps_calloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
QueueHandle_t xQueueCreateWithCaps(unsigned, size_t, unsigned);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
int xQueueSendToBack(QueueHandle_t, const void *, uint32_t);
void vQueueDelete(QueueHandle_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
unsigned uxQueueSpacesAvailable(QueueHandle_t);
void vTaskDelete(TaskHandle_t);
int xTaskCreateWithCaps(void (*)(void *), const char *, unsigned, void *,
                        unsigned, TaskHandle_t *, unsigned);
int64_t esp_timer_get_time(void);
void esp_crt_bundle_attach(void);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
void esp_http_client_cleanup(esp_http_client_handle_t);
#endif
