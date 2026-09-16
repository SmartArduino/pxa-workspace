#include "pxa_test_platform.h"
#include <setjmp.h>
#include <string.h>

static unsigned critical_depth, task_count, notifications;
static size_t allocation_count, live_allocations;
static int fail_allocation, fail_queue_send, cancel_in_transport, fail_transport;
static uint8_t queued[4];
static unsigned queued_count;
static jmp_buf worker_idle;

static void checked_free(void *memory) {
    assert(critical_depth == 0);
    if (memory != NULL) { assert(live_allocations != 0); --live_allocations; }
    free(memory);
}

#define ESP_PLATFORM 1
#define free checked_free
#include "../src/services/pxa_esp_net.c"
#undef free

void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void test_enter(void) { assert(critical_depth++ == 0); }
void test_leave(void) { assert(critical_depth-- == 1); }
void *heap_caps_malloc(size_t size, unsigned caps) {
    void *memory;
    (void)caps;
    assert(critical_depth == 0);
    if (fail_allocation) return NULL;
    memory = malloc(size);
    if (memory) { ++allocation_count; ++live_allocations; }
    return memory;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    void *memory = heap_caps_malloc(count * size, caps);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
QueueHandle_t xQueueCreateWithCaps(unsigned count, size_t size, unsigned caps) {
    (void)caps; assert(count == 4 && size == 1); return queued;
}
int xQueueReceive(QueueHandle_t queue, void *value, uint32_t timeout) {
    (void)queue; (void)timeout;
    if (queued_count == 0) longjmp(worker_idle, 1);
    *(uint8_t *)value = queued[0];
    memmove(queued, queued + 1, --queued_count);
    return pdTRUE;
}
int xQueueSendToBack(QueueHandle_t queue, const void *value, uint32_t timeout) {
    (void)queue; (void)timeout;
    if (fail_queue_send || queued_count == 4) return 0;
    queued[queued_count++] = *(const uint8_t *)value;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue) { (void)queue; }
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack,
    void *context, unsigned priority, TaskHandle_t *task, unsigned caps) {
    (void)fn; (void)name; (void)stack; (void)priority; (void)caps;
    ++task_count; *task = context; return pdPASS;
}
int64_t esp_timer_get_time(void) { return 1000; }
void esp_crt_bundle_attach(void) {}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    (void)c; assert(0); return NULL;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *n, const char *v) {
    (void)c; (void)n; (void)v; return ESP_FAIL;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *b, int n) {
    (void)c; (void)b; (void)n; return ESP_FAIL;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { (void)c; return ESP_FAIL; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return 0; }
void esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; }

int pxa_platform_net_http_request(const pxa_net_request_t *request,
    uint8_t *body, size_t capacity, size_t *size, uint16_t *status,
    char *content_type, size_t content_capacity, pxa_net_header_t *headers,
    size_t header_capacity, size_t *header_count, uint8_t *header_storage,
    size_t header_storage_capacity, size_t *header_bytes) {
    (void)headers; (void)header_capacity;
    assert(critical_depth == 0 && capacity >= 4 && content_capacity > 10);
    if (request->header_count) {
        assert(request->body.size == 3 && memcmp(request->body.data, "abc", 3) == 0);
        assert(header_storage_capacity == PXA_ESP_NET_RESPONSE_HEADER_BYTES);
        memset(header_storage, 0x5a, header_storage_capacity);
    }
    if (cancel_in_transport) cancel_request(NULL, g_net->next_operation);
    if (fail_transport) return PXA_PLATFORM_NET_FETCH_FAILED;
    memcpy(body, "test", 4); *size = 4; *status = 200;
    strcpy(content_type, "text/plain"); *header_count = 0; *header_bytes = 0;
    return PXA_PLATFORM_NET_FETCH_OK;
}

static void notified(void *context) { assert(context == &notifications); ++notifications; }
static void drain_worker(void) { if (setjmp(worker_idle) == 0) net_worker(g_net); }

int main(void) {
    pxa_net_backend_t backend;
    pxa_net_request_t request = {0};
    pxa_net_response_t response = {0};
    pxa_esp_net_snapshot_t snapshot;
    pxa_net_header_t header = {{(const uint8_t *)"x-test", 6}, {(const uint8_t *)"value", 5}};
    pxa_bytes_t wanted = {(const uint8_t *)"x-reply", 7};
    uint64_t operation;
    uint8_t bytes[8]; size_t size;
    assert(pxa_esp_net_backend(&backend, notified, &notifications));
    assert(allocation_count == 0 && task_count == 0);
    request.struct_size = sizeof(request);
    request.method = PXA_NET_METHOD_GET;
    request.abi_minor = 1;
    request.url = (pxa_bytes_t){(const uint8_t *)"https://test.invalid", 20};
    request.timeout_ms = 1000; request.max_response_bytes = 64;
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_OK);
    assert(task_count == 1 && live_allocations == 2);
    assert(backend.poll(NULL, operation, &response) == PXA_STATUS_WOULD_BLOCK);
    pxa_esp_net_snapshot(&snapshot);
    assert(snapshot.slot_storage_bytes == sizeof(g_net->slots) + 64);
    drain_worker();
    assert(notifications == 1);
    assert(backend.poll(NULL, operation, &response) == PXA_STATUS_OK);
    assert(backend.read_body(NULL, response.body_stream, bytes, 2, &size) == PXA_STATUS_OK);
    assert(size == 2 && memcmp(bytes, "te", 2) == 0);
    backend.close_body(NULL, response.body_stream);
    assert(live_allocations == 1);

    fail_allocation = 1;
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_RESOURCE_LIMIT);
    fail_allocation = 0; fail_queue_send = 1;
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_RESOURCE_LIMIT);
    fail_queue_send = 0;
    assert(live_allocations == 1);
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_OK);
    backend.cancel(NULL, operation); drain_worker();
    assert(live_allocations == 1);
    cancel_in_transport = 1;
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_OK);
    drain_worker(); cancel_in_transport = 0;
    assert(live_allocations == 1);
    fail_transport = 1;
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_OK);
    drain_worker(); fail_transport = 0;
    assert(backend.poll(NULL, operation, &response) == PXA_STATUS_UNAVAILABLE);
    assert(live_allocations == 1);

    request.headers = &header; request.header_count = 1;
    request.wanted_response_headers = &wanted; request.wanted_response_header_count = 1;
    request.body = (pxa_bytes_t){(const uint8_t *)"abc", 3};
    for (unsigned i = 0; i < 4; ++i)
        assert(backend.start(NULL, &request, &operation) == PXA_STATUS_OK);
    assert(backend.start(NULL, &request, &operation) == PXA_STATUS_RESOURCE_LIMIT);
    drain_worker();
    pxa_esp_net_reset_requests();
    assert(live_allocations == 1 && task_count == 1);
    pxa_esp_net_snapshot(&snapshot);
    assert(snapshot.active_slots == 0 && snapshot.slot_storage_bytes == sizeof(g_net->slots));
    checked_free(g_net); g_net = NULL;
    assert(live_allocations == 0);
    return 0;
}
