#include "pxa_esp_net.h"

#if defined(ESP_PLATFORM)

#include "sdkconfig.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pxa/pxa_platform_net.h"

#define PXA_ESP_NET_TAG "PxaNet"
#define PXA_ESP_NET_URL_CAPACITY 513
#define PXA_ESP_NET_CONTENT_TYPE_CAPACITY 97
#define PXA_ESP_NET_MAX_HEADERS 8
#define PXA_ESP_NET_WANTED_HEADER_BYTES 520
#define PXA_ESP_NET_TASK_PRIORITY 3

#ifndef CONFIG_PXA_NET_WORKER_STACK_SIZE
#define CONFIG_PXA_NET_WORKER_STACK_SIZE (8 * 1024)
#endif

typedef enum {
    PXA_ESP_NET_SLOT_FREE = 0,
    PXA_ESP_NET_SLOT_RESERVED,
    PXA_ESP_NET_SLOT_QUEUED,
    PXA_ESP_NET_SLOT_RUNNING,
    PXA_ESP_NET_SLOT_COMPLETE,
    PXA_ESP_NET_SLOT_STREAM,
} pxa_esp_net_slot_state_t;

typedef struct {
    uint64_t operation;
    size_t body_size;
    size_t body_offset;
    uint64_t deadline_us;
    uint32_t max_response_bytes;
    uint32_t timeout_ms;
    uint16_t status_code;
    uint16_t method;
    uint16_t abi_minor;
    uint16_t request_header_count;
    uint16_t wanted_header_count;
    uint16_t response_header_count;
    uint16_t request_body_size;
    uint16_t response_header_bytes;
    pxa_status_t result;
    uint8_t state;
    uint8_t cancelled;
    uint8_t *body;
    uint8_t *request_body;
    size_t storage_bytes;
    char *request_header_data;
    char *wanted_header_data;
    char *response_header_data;
    pxa_net_header_t request_headers[PXA_ESP_NET_MAX_HEADERS];
    pxa_bytes_t wanted_headers[PXA_ESP_NET_MAX_HEADERS];
    pxa_net_header_t response_headers[PXA_ESP_NET_MAX_HEADERS];
    char url[PXA_ESP_NET_URL_CAPACITY];
    char content_type[PXA_ESP_NET_CONTENT_TYPE_CAPACITY];
} pxa_esp_net_slot_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    uint64_t next_operation;
    pxa_esp_net_notify_fn notify;
    void *notify_context;
    pxa_esp_net_slot_t slots[PXA_ESP_NET_MAX_PENDING];
    pxa_esp_net_snapshot_t metrics;
} pxa_esp_net_state_t;

static pxa_esp_net_state_t *g_net;
static pxa_esp_net_notify_fn g_lazy_notify;
static void *g_lazy_notify_context;
static portMUX_TYPE g_net_lock = portMUX_INITIALIZER_UNLOCKED;

extern int pxa_platform_net_fetch_get(
    const char *url, uint32_t timeout_ms, uint8_t *body, size_t body_capacity,
    size_t *body_size, uint16_t *status_code, char *content_type,
    size_t content_type_capacity) __attribute__((weak));
extern int pxa_platform_net_http_request(
    const pxa_net_request_t *request, uint8_t *body, size_t body_capacity,
    size_t *body_size, uint16_t *status_code, char *content_type,
    size_t content_type_capacity, pxa_net_header_t *response_headers,
    size_t response_header_capacity, size_t *response_header_count,
    uint8_t *header_storage, size_t header_storage_capacity,
    size_t *header_storage_size) __attribute__((weak));

static void *reset_slot_metadata(pxa_esp_net_slot_t *slot) {
    void *storage = slot->body;
    memset(slot, 0, offsetof(pxa_esp_net_slot_t, body));
    slot->body = NULL;
    slot->request_body = NULL;
    slot->storage_bytes = 0;
    slot->state = PXA_ESP_NET_SLOT_FREE;
    /* The caller frees detached storage after leaving the critical section. */
    return storage;
}

static pxa_esp_net_slot_t *find_slot(pxa_esp_net_state_t *net,
                                     uint64_t operation) {
    uint16_t index;
    for (index = 0; index < PXA_ESP_NET_MAX_PENDING; ++index) {
        pxa_esp_net_slot_t *slot = &net->slots[index];
        if (slot->state != PXA_ESP_NET_SLOT_FREE &&
            slot->operation == operation) {
            return slot;
        }
    }
    return NULL;
}

static int owns_slot(const pxa_esp_net_state_t *net,
                     const pxa_esp_net_slot_t *slot) {
    const uintptr_t address = (uintptr_t)slot;
    const uintptr_t begin = (uintptr_t)&net->slots[0];
    const uintptr_t end = (uintptr_t)&net->slots[PXA_ESP_NET_MAX_PENDING];
    return address >= begin && address < end &&
           (address - begin) % sizeof(net->slots[0]) == 0;
}

static uint16_t active_slot_count_locked(const pxa_esp_net_state_t *net) {
    uint16_t count = 0;
    uint16_t index;
    for (index = 0; index < PXA_ESP_NET_MAX_PENDING; ++index) {
        if (net->slots[index].state != PXA_ESP_NET_SLOT_FREE) count++;
    }
    return count;
}

static int header_name_equal(const char *header, pxa_bytes_t wanted) {
    size_t length = 0;
    if (header == NULL || wanted.data == NULL) return 0;
    while (header[length] != '\0' && length <= wanted.size) ++length;
    return length == wanted.size &&
           strncasecmp(header, (const char *)wanted.data, wanted.size) == 0;
}

static int response_header_already_captured(const pxa_esp_net_slot_t *slot,
                                            pxa_bytes_t name) {
    uint16_t index;
    for (index = 0; index < slot->response_header_count; ++index) {
        if (slot->response_headers[index].name.size == name.size &&
            memcmp(slot->response_headers[index].name.data, name.data,
                   name.size) == 0) {
            return 1;
        }
    }
    return 0;
}

static int capture_response_header(pxa_esp_net_slot_t *slot,
                                   const char *name, const char *value) {
    uint16_t index;
    for (index = 0; index < slot->wanted_header_count; ++index) {
        pxa_bytes_t wanted = slot->wanted_headers[index];
        size_t length = 0;
        char *destination;
        if (!header_name_equal(name, wanted) ||
            response_header_already_captured(slot, wanted)) {
            continue;
        }
        while (value[length] != '\0' &&
               length <= PXA_NET_MAX_HEADER_VALUE_BYTES) {
            ++length;
        }
        if (length > PXA_NET_MAX_HEADER_VALUE_BYTES ||
            length + 1u > PXA_ESP_NET_RESPONSE_HEADER_BYTES -
                              slot->response_header_bytes ||
            slot->response_header_count >= PXA_ESP_NET_MAX_HEADERS) {
            return 0;
        }
        destination = slot->response_header_data + slot->response_header_bytes;
        memcpy(destination, value, length);
        destination[length] = '\0';
        slot->response_headers[slot->response_header_count].name = wanted;
        slot->response_headers[slot->response_header_count].value =
            (pxa_bytes_t){(const uint8_t *)destination, length};
        slot->response_header_count++;
        slot->response_header_bytes += (uint16_t)(length + 1u);
        return 1;
    }
    return 1;
}

static esp_err_t http_event(esp_http_client_event_t *event) {
    pxa_esp_net_slot_t *slot;
    if (event == NULL || event->user_data == NULL) return ESP_OK;
    slot = (pxa_esp_net_slot_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != NULL &&
        event->header_value != NULL) {
        size_t length;
        portENTER_CRITICAL(&g_net_lock);
        if (slot->cancelled) {
            portEXIT_CRITICAL(&g_net_lock);
            return ESP_FAIL;
        }
        portEXIT_CRITICAL(&g_net_lock);
        if (strcasecmp(event->header_key, "Content-Type") == 0) {
            for (length = 0; length < PXA_ESP_NET_CONTENT_TYPE_CAPACITY;
                 ++length) {
                if (event->header_value[length] == '\0') break;
            }
            if (length == PXA_ESP_NET_CONTENT_TYPE_CAPACITY) {
                slot->result = PXA_STATUS_LIMIT_EXCEEDED;
                return ESP_ERR_NO_MEM;
            }
            memcpy(slot->content_type, event->header_value, length);
            slot->content_type[length] = '\0';
        }
        if (!capture_response_header(slot, event->header_key,
                                     event->header_value)) {
            slot->result = PXA_STATUS_LIMIT_EXCEEDED;
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }
    {
        size_t body_offset;
        portENTER_CRITICAL(&g_net_lock);
        if (slot->cancelled) {
            portEXIT_CRITICAL(&g_net_lock);
            return ESP_FAIL;
        }
        if ((size_t)event->data_len >
            slot->max_response_bytes - slot->body_size) {
            slot->result = PXA_STATUS_LIMIT_EXCEEDED;
            portEXIT_CRITICAL(&g_net_lock);
            return ESP_ERR_NO_MEM;
        }
        body_offset = slot->body_size;
        slot->body_size += (size_t)event->data_len;
        portEXIT_CRITICAL(&g_net_lock);
        memcpy(slot->body + body_offset, event->data,
               (size_t)event->data_len);
    }
    return ESP_OK;
}

static const char *method_name(uint16_t method) {
    switch (method) {
    case PXA_NET_METHOD_GET: return "GET";
    case PXA_NET_METHOD_HEAD: return "HEAD";
    case PXA_NET_METHOD_POST: return "POST";
    case PXA_NET_METHOD_PUT: return "PUT";
    case PXA_NET_METHOD_PATCH: return "PATCH";
    case PXA_NET_METHOD_DELETE: return "DELETE";
    default: return NULL;
    }
}

static esp_http_client_method_t esp_method(uint16_t method) {
    switch (method) {
    case PXA_NET_METHOD_HEAD: return HTTP_METHOD_HEAD;
    case PXA_NET_METHOD_POST: return HTTP_METHOD_POST;
    case PXA_NET_METHOD_PUT: return HTTP_METHOD_PUT;
    case PXA_NET_METHOD_PATCH: return HTTP_METHOD_PATCH;
    case PXA_NET_METHOD_DELETE: return HTTP_METHOD_DELETE;
    case PXA_NET_METHOD_GET:
    default: return HTTP_METHOD_GET;
    }
}

static void net_worker(void *context) {
    pxa_esp_net_state_t *net = (pxa_esp_net_state_t *)context;
    uint8_t index;
    for (;;) {
        esp_http_client_config_t config;
        esp_http_client_handle_t client;
        esp_err_t error;
        int status_code;
        int platform_result;
        int notify_ready = 0;
        int cancelled = 0;
        void *released_storage = NULL;
        pxa_esp_net_notify_fn notify = NULL;
        void *notify_context = NULL;
        pxa_status_t result = PXA_STATUS_INTERNAL;
        size_t body_size = 0;
        uint64_t operation = 0;
        uint16_t header_index;
        uint16_t method;
        size_t platform_header_count = 0;
        size_t platform_header_bytes = 0;
        pxa_esp_net_slot_t *slot;

        if (xQueueReceive(net->queue, &index, portMAX_DELAY) != pdTRUE ||
            index >= PXA_ESP_NET_MAX_PENDING) {
            continue;
        }
        slot = &net->slots[index];
        portENTER_CRITICAL(&g_net_lock);
        if (slot->state != PXA_ESP_NET_SLOT_QUEUED) {
            portEXIT_CRITICAL(&g_net_lock);
            continue;
        }
        if (slot->cancelled) {
            net->metrics.cancelled_requests++;
            released_storage = reset_slot_metadata(slot);
            portEXIT_CRITICAL(&g_net_lock);
            free(released_storage);
            continue;
        }
        slot->state = PXA_ESP_NET_SLOT_RUNNING;
        slot->deadline_us = (uint64_t)esp_timer_get_time() +
                            (uint64_t)slot->timeout_ms * 1000u;
        operation = slot->operation;
        method = slot->method;
        portEXIT_CRITICAL(&g_net_lock);

        ESP_LOGI(PXA_ESP_NET_TAG, "Network %s started: %s",
                 method_name(method), slot->url);
        platform_result = PXA_PLATFORM_NET_FETCH_NOT_USED;
        if (pxa_platform_net_http_request != NULL) {
            pxa_net_request_t request;
            memset(&request, 0, sizeof(request));
            request.struct_size = sizeof(request);
            request.url = (pxa_bytes_t){(const uint8_t *)slot->url,
                                        strlen(slot->url)};
            request.method = slot->method;
            request.max_response_bytes = slot->max_response_bytes;
            request.timeout_ms = slot->timeout_ms;
            request.headers = slot->request_headers;
            request.header_count = slot->request_header_count;
            request.body = (pxa_bytes_t){slot->request_body,
                                         slot->request_body_size};
            request.wanted_response_headers = slot->wanted_headers;
            request.wanted_response_header_count = slot->wanted_header_count;
            request.abi_minor = slot->abi_minor;
            platform_result = pxa_platform_net_http_request(
                &request, slot->body, slot->max_response_bytes,
                &slot->body_size, &slot->status_code, slot->content_type,
                sizeof(slot->content_type), slot->response_headers,
                PXA_ESP_NET_MAX_HEADERS, &platform_header_count,
                (uint8_t *)slot->response_header_data,
                slot->wanted_header_count == 0 ? 0 :
                    PXA_ESP_NET_RESPONSE_HEADER_BYTES, &platform_header_bytes);
            if (platform_header_count <= PXA_ESP_NET_MAX_HEADERS &&
                platform_header_bytes <= UINT16_MAX) {
                slot->response_header_count =
                    (uint16_t)platform_header_count;
                slot->response_header_bytes =
                    (uint16_t)platform_header_bytes;
            } else {
                platform_result = PXA_PLATFORM_NET_FETCH_RESPONSE_TOO_LARGE;
            }
        } else if (slot->method == PXA_NET_METHOD_GET &&
                   slot->request_header_count == 0 &&
                   slot->request_body_size == 0 &&
                   slot->wanted_header_count == 0 &&
                   pxa_platform_net_fetch_get != NULL) {
            platform_result = pxa_platform_net_fetch_get(
                slot->url, slot->timeout_ms, slot->body,
                slot->max_response_bytes, &slot->body_size, &slot->status_code,
                slot->content_type, sizeof(slot->content_type));
        }
        if (platform_result == PXA_PLATFORM_NET_FETCH_NOT_USED) {
            memset(&config, 0, sizeof(config));
            config.url = slot->url;
            config.method = esp_method(slot->method);
            config.timeout_ms = (int)slot->timeout_ms;
            config.disable_auto_redirect = true;
            config.buffer_size = 512;
            config.buffer_size_tx = 512;
            config.event_handler = http_event;
            config.user_data = slot;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            client = esp_http_client_init(&config);
            if (client == NULL) {
                error = ESP_ERR_NO_MEM;
                status_code = 0;
            } else {
                error = ESP_OK;
                for (header_index = 0;
                     header_index < slot->request_header_count;
                     ++header_index) {
                    const pxa_net_header_t *header =
                        &slot->request_headers[header_index];
                    error = esp_http_client_set_header(
                        client, (const char *)header->name.data,
                        (const char *)header->value.data);
                    if (error != ESP_OK) break;
                }
                if (error == ESP_OK && slot->request_body_size != 0) {
                    error = esp_http_client_set_post_field(
                        client, (const char *)slot->request_body,
                        slot->request_body_size);
                }
                if (error == ESP_OK) error = esp_http_client_perform(client);
                status_code = esp_http_client_get_status_code(client);
                esp_http_client_cleanup(client);
            }
        } else if (platform_result == PXA_PLATFORM_NET_FETCH_OK) {
            error = ESP_OK;
            status_code = slot->status_code;
        } else {
            if (platform_result == PXA_PLATFORM_NET_FETCH_RESPONSE_TOO_LARGE) {
                slot->result = PXA_STATUS_LIMIT_EXCEEDED;
                error = ESP_ERR_NO_MEM;
            } else {
                error = ESP_FAIL;
            }
            status_code = 0;
        }

        portENTER_CRITICAL(&g_net_lock);
        if (slot->cancelled) {
            cancelled = 1;
            net->metrics.cancelled_requests++;
            released_storage = reset_slot_metadata(slot);
        } else {
            if (slot->result == PXA_STATUS_LIMIT_EXCEEDED) {
                /* Some transports ignore an event callback's abort result. */
            } else if ((error == ESP_OK || slot->method == PXA_NET_METHOD_HEAD) &&
                       status_code >= 100 && status_code <= 599) {
                slot->status_code = (uint16_t)status_code;
                slot->result = PXA_STATUS_OK;
            } else if (error == ESP_ERR_TIMEOUT ||
                       (error != ESP_OK &&
                        (uint64_t)esp_timer_get_time() >= slot->deadline_us)) {
                slot->result = PXA_STATUS_TIMED_OUT;
            } else if (error == ESP_ERR_NO_MEM) {
                slot->result = PXA_STATUS_RESOURCE_LIMIT;
            } else {
                slot->result = PXA_STATUS_UNAVAILABLE;
            }
            slot->state = PXA_ESP_NET_SLOT_COMPLETE;
            result = slot->result;
            body_size = slot->body_size;
            net->metrics.completed_requests++;
            net->metrics.response_bytes += body_size;
            if (body_size > net->metrics.peak_response_bytes) {
                net->metrics.peak_response_bytes = (uint32_t)body_size;
            }
            notify_ready = 1;
            notify = net->notify;
            notify_context = net->notify_context;
        }
        portEXIT_CRITICAL(&g_net_lock);
        free(released_storage);
        if (cancelled) {
            ESP_LOGW(PXA_ESP_NET_TAG,
                     "Network %s cancelled: operation=%llu",
                     method_name(method), (unsigned long long)operation);
        } else {
            ESP_LOGI(PXA_ESP_NET_TAG,
                     "Network %s completed: operation=%llu error=%d "
                     "status=%d result=%d bytes=%u",
                     method_name(method), (unsigned long long)operation,
                     (int)error, status_code, (int)result,
                     (unsigned)body_size);
        }
        if (notify_ready && notify != NULL) {
            notify(notify_context);
        }
    }
}

static pxa_esp_net_state_t *get_state(pxa_esp_net_notify_fn notify,
                                      void *notify_context) {
    pxa_esp_net_state_t *net;
    TaskHandle_t task = NULL;
    if (g_net != NULL) {
        portENTER_CRITICAL(&g_net_lock);
        g_net->notify = notify;
        g_net->notify_context = notify_context;
        portEXIT_CRITICAL(&g_net_lock);
        return g_net;
    }
    net = heap_caps_calloc(1, sizeof(*net),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (net == NULL) return NULL;
    net->notify = notify;
    net->notify_context = notify_context;
    net->metrics.slot_storage_bytes = sizeof(net->slots);
    net->queue = xQueueCreateWithCaps(PXA_ESP_NET_MAX_PENDING,
                                      sizeof(uint8_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (net->queue == NULL ||
        xTaskCreateWithCaps(net_worker, "pxa_net",
                            CONFIG_PXA_NET_WORKER_STACK_SIZE, net,
                            PXA_ESP_NET_TASK_PRIORITY, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (net->queue != NULL) vQueueDelete(net->queue);
        free(net);
        return NULL;
    }
    net->task = task;
    g_net = net;
    return net;
}

static pxa_status_t start_request(void *context,
                                  const pxa_net_request_t *request,
                                  uint64_t *operation) {
    pxa_esp_net_state_t *net = g_net;
    pxa_esp_net_slot_t *slot = NULL;
    uint64_t value;
    uint8_t index;
    uint16_t header_index;
    uint16_t active_slots;
    size_t request_header_used = 0;
    size_t wanted_header_used = 0;
    size_t storage_bytes;
    (void)context;
    if (request == NULL || operation == NULL ||
        request->struct_size < sizeof(*request) ||
        request->method < PXA_NET_METHOD_GET ||
        request->method > PXA_NET_METHOD_DELETE ||
        request->url.data == NULL || request->url.size == 0 ||
        request->url.size >= PXA_ESP_NET_URL_CAPACITY ||
        request->max_response_bytes == 0 ||
        request->max_response_bytes > PXA_ESP_NET_MAX_RESPONSE_BYTES ||
        request->timeout_ms < 100 || request->timeout_ms > 60000 ||
        request->header_count > PXA_ESP_NET_MAX_HEADERS ||
        request->wanted_response_header_count > PXA_ESP_NET_MAX_HEADERS ||
        request->body.size > PXA_ESP_NET_MAX_INLINE_BODY_BYTES ||
        (request->header_count != 0 && request->headers == NULL) ||
        (request->wanted_response_header_count != 0 &&
         request->wanted_response_headers == NULL) ||
        (request->body.size != 0 && request->body.data == NULL)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    for (header_index = 0; header_index < request->header_count;
         ++header_index) {
        const pxa_net_header_t *header = &request->headers[header_index];
        size_t needed = header->name.size + header->value.size + 2u;
        if (!pxa_net_header_name_valid(header->name, 1) ||
            !pxa_net_header_value_valid(header->value) ||
            needed > PXA_ESP_NET_REQUEST_HEADER_BYTES -
                         request_header_used) {
            return PXA_STATUS_INVALID_ARGUMENT;
        }
        request_header_used += needed;
    }
    for (header_index = 0;
         header_index < request->wanted_response_header_count;
         ++header_index) {
        pxa_bytes_t name = request->wanted_response_headers[header_index];
        if (!pxa_net_header_name_valid(name, 0) ||
            name.size + 1u > PXA_ESP_NET_WANTED_HEADER_BYTES -
                                 wanted_header_used) {
            return PXA_STATUS_INVALID_ARGUMENT;
        }
        wanted_header_used += name.size + 1u;
    }
    if (net == NULL) net = get_state(g_lazy_notify, g_lazy_notify_context);
    if (net == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    portENTER_CRITICAL(&g_net_lock);
    for (index = 0; index < PXA_ESP_NET_MAX_PENDING; ++index) {
        if (net->slots[index].state == PXA_ESP_NET_SLOT_FREE) {
            slot = &net->slots[index];
            break;
        }
    }
    if (slot == NULL) {
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    value = ++net->next_operation;
    if (value == 0) value = ++net->next_operation;
    reset_slot_metadata(slot);
    slot->operation = value;
    slot->state = PXA_ESP_NET_SLOT_RESERVED;
    active_slots = active_slot_count_locked(net);
    if (active_slots > net->metrics.peak_active_slots) {
        net->metrics.peak_active_slots = active_slots;
    }
    portEXIT_CRITICAL(&g_net_lock);

    storage_bytes = request->max_response_bytes + request->body.size +
                    request_header_used + wanted_header_used +
                    (request->wanted_response_header_count == 0 ? 0 :
                     PXA_ESP_NET_RESPONSE_HEADER_BYTES);
    slot->body = heap_caps_malloc(storage_bytes,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (slot->body == NULL) {
        portENTER_CRITICAL(&g_net_lock);
        (void)reset_slot_metadata(slot);
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    slot->request_body = slot->body + request->max_response_bytes;
    slot->request_header_data = (char *)slot->request_body + request->body.size;
    slot->wanted_header_data = slot->request_header_data + request_header_used;
    slot->response_header_data = slot->wanted_header_data + wanted_header_used;
    portENTER_CRITICAL(&g_net_lock);
    slot->storage_bytes = storage_bytes;
    portEXIT_CRITICAL(&g_net_lock);
    slot->max_response_bytes = request->max_response_bytes;
    slot->timeout_ms = request->timeout_ms;
    slot->method = request->method;
    slot->abi_minor = request->abi_minor;
    slot->request_header_count = request->header_count;
    slot->wanted_header_count = request->wanted_response_header_count;
    slot->request_body_size = (uint16_t)request->body.size;
    slot->result = PXA_STATUS_WOULD_BLOCK;
    memcpy(slot->url, request->url.data, request->url.size);
    slot->url[request->url.size] = '\0';
    if (request->body.size != 0) {
        memcpy(slot->request_body, request->body.data, request->body.size);
    }
    request_header_used = 0;
    for (header_index = 0; header_index < request->header_count;
         ++header_index) {
        const pxa_net_header_t *source = &request->headers[header_index];
        char *name = slot->request_header_data + request_header_used;
        char *header_value;
        memcpy(name, source->name.data, source->name.size);
        name[source->name.size] = '\0';
        request_header_used += source->name.size + 1u;
        header_value = slot->request_header_data + request_header_used;
        memcpy(header_value, source->value.data, source->value.size);
        header_value[source->value.size] = '\0';
        request_header_used += source->value.size + 1u;
        slot->request_headers[header_index].name =
            (pxa_bytes_t){(const uint8_t *)name, source->name.size};
        slot->request_headers[header_index].value =
            (pxa_bytes_t){(const uint8_t *)header_value, source->value.size};
    }
    wanted_header_used = 0;
    for (header_index = 0;
         header_index < request->wanted_response_header_count;
         ++header_index) {
        pxa_bytes_t source = request->wanted_response_headers[header_index];
        char *name = slot->wanted_header_data + wanted_header_used;
        memcpy(name, source.data, source.size);
        name[source.size] = '\0';
        wanted_header_used += source.size + 1u;
        slot->wanted_headers[header_index] =
            (pxa_bytes_t){(const uint8_t *)name, source.size};
    }
    portENTER_CRITICAL(&g_net_lock);
    if (slot->state != PXA_ESP_NET_SLOT_RESERVED ||
        slot->operation != value) {
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_CANCELLED;
    }
    slot->state = PXA_ESP_NET_SLOT_QUEUED;
    portEXIT_CRITICAL(&g_net_lock);
    if (xQueueSendToBack(net->queue, &index, 0) != pdTRUE) {
        void *storage = NULL;
        portENTER_CRITICAL(&g_net_lock);
        net->metrics.queue_full_requests++;
        if (slot->state == PXA_ESP_NET_SLOT_QUEUED &&
            slot->operation == value) {
            storage = reset_slot_metadata(slot);
        }
        portEXIT_CRITICAL(&g_net_lock);
        free(storage);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    portENTER_CRITICAL(&g_net_lock);
    net->metrics.started_requests++;
    portEXIT_CRITICAL(&g_net_lock);
    *operation = value;
    return PXA_STATUS_OK;
}

static pxa_status_t poll_request(void *context, uint64_t operation,
                                 pxa_net_response_t *response) {
    pxa_esp_net_state_t *net = g_net;
    pxa_esp_net_slot_t *slot;
    (void)context;
    if (net == NULL || net != g_net || response == NULL || operation == 0) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&g_net_lock);
    slot = find_slot(net, operation);
    if (slot == NULL) {
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_NOT_FOUND;
    }
    if (slot->state == PXA_ESP_NET_SLOT_RESERVED ||
        slot->state == PXA_ESP_NET_SLOT_QUEUED) {
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_WOULD_BLOCK;
    }
    if (slot->state == PXA_ESP_NET_SLOT_RUNNING) {
        if ((uint64_t)esp_timer_get_time() >= slot->deadline_us) {
            if (!slot->cancelled) {
                slot->cancelled = 1;
                net->metrics.timed_out_requests++;
            }
            portEXIT_CRITICAL(&g_net_lock);
            ESP_LOGW(PXA_ESP_NET_TAG,
                     "Network request timed out: operation=%llu",
                     (unsigned long long)operation);
            return PXA_STATUS_TIMED_OUT;
        }
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_WOULD_BLOCK;
    }
    if (slot->state != PXA_ESP_NET_SLOT_COMPLETE) {
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (slot->result != PXA_STATUS_OK) {
        pxa_status_t result = slot->result;
        void *storage = reset_slot_metadata(slot);
        portEXIT_CRITICAL(&g_net_lock);
        free(storage);
        return result;
    }
    response->status_code = slot->status_code;
    response->content_type.data = (const uint8_t *)slot->content_type;
    response->content_type.size = strlen(slot->content_type);
    response->body_stream = slot;
    if (slot->abi_minor >= 1) {
        response->body_length = slot->body_size;
        response->flags = PXA_NET_RESPONSE_BODY_LENGTH_KNOWN;
        if (slot->method != PXA_NET_METHOD_HEAD && slot->body_size != 0) {
            response->flags |= PXA_NET_RESPONSE_BODY_PRESENT;
        }
        response->headers = slot->response_headers;
        response->header_count = slot->response_header_count;
    }
    slot->state = PXA_ESP_NET_SLOT_STREAM;
    portEXIT_CRITICAL(&g_net_lock);
    return PXA_STATUS_OK;
}

static void cancel_request(void *context, uint64_t operation) {
    pxa_esp_net_state_t *net = g_net;
    pxa_esp_net_slot_t *slot;
    void *storage = NULL;
    (void)context;
    if (net == NULL || net != g_net || operation == 0) return;
    portENTER_CRITICAL(&g_net_lock);
    slot = find_slot(net, operation);
    if (slot != NULL) {
        if (slot->state == PXA_ESP_NET_SLOT_COMPLETE ||
            slot->state == PXA_ESP_NET_SLOT_STREAM ||
            slot->state == PXA_ESP_NET_SLOT_RESERVED) {
            net->metrics.cancelled_requests++;
            storage = reset_slot_metadata(slot);
        } else if (!slot->cancelled) {
            slot->cancelled = 1;
        }
    }
    portEXIT_CRITICAL(&g_net_lock);
    free(storage);
}

static pxa_status_t read_body(void *context, void *body_stream,
                              uint8_t *output, size_t capacity, size_t *size) {
    pxa_esp_net_state_t *net = g_net;
    pxa_esp_net_slot_t *slot = (pxa_esp_net_slot_t *)body_stream;
    const uint8_t *source;
    size_t count;
    (void)context;
    if (net == NULL || net != g_net || slot == NULL || size == NULL ||
        (output == NULL && capacity != 0)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    *size = 0;
    portENTER_CRITICAL(&g_net_lock);
    if (!owns_slot(net, slot) || slot->state != PXA_ESP_NET_SLOT_STREAM) {
        portEXIT_CRITICAL(&g_net_lock);
        return PXA_STATUS_BAD_STATE;
    }
    count = slot->body_size - slot->body_offset;
    if (count > capacity) count = capacity;
    source = slot->body + slot->body_offset;
    slot->body_offset += count;
    portEXIT_CRITICAL(&g_net_lock);
    /* STREAM slots are owned by the Core owner thread; the worker no longer
     * writes them after publishing COMPLETE. Avoid copying PSRAM while the
     * cross-core metadata lock has interrupts disabled. */
    if (count != 0) memcpy(output, source, count);
    *size = count;
    return PXA_STATUS_OK;
}

static void close_body(void *context, void *body_stream) {
    pxa_esp_net_state_t *net = g_net;
    pxa_esp_net_slot_t *slot = (pxa_esp_net_slot_t *)body_stream;
    void *storage = NULL;
    (void)context;
    if (net == NULL || net != g_net || slot == NULL) return;
    portENTER_CRITICAL(&g_net_lock);
    if (owns_slot(net, slot) && slot->state == PXA_ESP_NET_SLOT_STREAM) {
        storage = reset_slot_metadata(slot);
    }
    portEXIT_CRITICAL(&g_net_lock);
    free(storage);
}

int pxa_esp_net_backend(pxa_net_backend_t *output,
                        pxa_esp_net_notify_fn notify, void *notify_context) {
    if (output == NULL || notify == NULL) return 0;
    memset(output, 0, sizeof(*output));
    g_lazy_notify = notify;
    g_lazy_notify_context = notify_context;
    if (g_net != NULL) (void)get_state(notify, notify_context);
    output->struct_size = sizeof(*output);
    output->start = start_request;
    output->poll = poll_request;
    output->cancel = cancel_request;
    output->read_body = read_body;
    output->close_body = close_body;
    return 1;
}

void pxa_esp_net_reset_requests(void) {
    uint16_t index;
    void *storage[PXA_ESP_NET_MAX_PENDING] = {0};
    if (g_net == NULL) return;
    portENTER_CRITICAL(&g_net_lock);
    for (index = 0; index < PXA_ESP_NET_MAX_PENDING; ++index) {
        pxa_esp_net_slot_t *slot = &g_net->slots[index];
        if (slot->state == PXA_ESP_NET_SLOT_QUEUED ||
            slot->state == PXA_ESP_NET_SLOT_RUNNING) {
            if (!slot->cancelled) slot->cancelled = 1;
        } else if (slot->state != PXA_ESP_NET_SLOT_FREE) {
            g_net->metrics.cancelled_requests++;
            storage[index] = reset_slot_metadata(slot);
        }
    }
    portEXIT_CRITICAL(&g_net_lock);
    for (index = 0; index < PXA_ESP_NET_MAX_PENDING; ++index) free(storage[index]);
}

void pxa_esp_net_snapshot(pxa_esp_net_snapshot_t *output) {
    uint16_t index;
    if (output == NULL) return;
    memset(output, 0, sizeof(*output));
    if (g_net == NULL) return;
    portENTER_CRITICAL(&g_net_lock);
    *output = g_net->metrics;
    for (index = 0; index < PXA_ESP_NET_MAX_PENDING; ++index)
        output->slot_storage_bytes += g_net->slots[index].storage_bytes;
    output->active_slots = active_slot_count_locked(g_net);
    portEXIT_CRITICAL(&g_net_lock);
}

#endif
