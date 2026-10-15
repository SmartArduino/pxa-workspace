#include "pxa_test_platform.h"
#include <setjmp.h>
#include <string.h>

static unsigned critical_depth, task_count, notifications;
static unsigned worker_notifications, worker_pending_notifications, worker_waits;
static size_t allocation_count, live_allocations;
static int fail_allocation, fail_queue_send, cancel_in_transport, fail_transport;
static uint8_t queued[4];
static unsigned queued_count;
static jmp_buf worker_idle;
static esp_http_client_config_t mock_client;
static unsigned mock_offset, mock_size, mock_uploaded, mock_cleanups, mock_header_waits, mock_inits;
static unsigned mock_mode;
static int mock_status;
static int64_t mock_time = 1000;

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
    assert(critical_depth == 0);
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
int64_t esp_timer_get_time(void) { return mock_time; }
void esp_crt_bundle_attach(void) {}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    assert(critical_depth == 0);
    ++mock_inits; mock_client = *c; mock_offset = mock_uploaded = 0;
    mock_size = mock_mode == 2 ? 0 : 8192;
    mock_status = mock_mode == 2 ? 413 : 200;
    assert(c->disable_auto_redirect && c->buffer_size == 512);
    return &mock_client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *n, const char *v) {
    (void)c; (void)n; (void)v; return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *b, int n) {
    (void)c; (void)b; (void)n; return ESP_FAIL;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { (void)c; return ESP_FAIL; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return mock_status; }
void esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; ++mock_cleanups; }

void vTaskDelay(TickType_t ticks) { (void)ticks; }
void xTaskNotifyGive(TaskHandle_t task) {
    assert(task == g_net->task && critical_depth == 0);
    ++worker_notifications;
    ++worker_pending_notifications;
}
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks) {
    assert(clear == pdTRUE && ticks == 1 && critical_depth == 0);
    unsigned pending = worker_pending_notifications;
    worker_pending_notifications = 0;
    ++worker_waits;
    return pending;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) { (void)c; (void)n; return ESP_OK; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int n) { (void)c; (void)n; return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t c, const char *s, int n) {
    (void)c;
    for (int i = 0; i < n; ++i) assert((unsigned char)s[i] == ((mock_uploaded + i) & 255));
    mock_uploaded += n;
    return n;
}
static void mock_header(char *name, char *value) {
    esp_http_client_event_t event = {0};
    event.user_data = mock_client.user_data; event.event_id = HTTP_EVENT_ON_HEADER;
    event.header_key = name; event.header_value = value;
    (void)mock_client.event_handler(&event);
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    (void)c;
    if (mock_header_waits) { --mock_header_waits; return -ESP_ERR_HTTP_EAGAIN; }
    if (mock_mode != 1) mock_header("Content-Length", mock_size ? "8192" : "0");
    else mock_header("Transfer-Encoding", "chunked");
    mock_header("Set-Cookie", "a=1"); mock_header("Set-Cookie", "b=2");
    if (mock_mode == 3) mock_header("Content-Length", "8193");
    return mock_size;
}
int esp_http_client_read(esp_http_client_handle_t c, char *s, int n) {
    (void)c;
    if ((unsigned)n > mock_size - mock_offset) n = (int)(mock_size - mock_offset);
    for (int i = 0; i < n; ++i) s[i] = (char)((mock_offset + i) & 255);
    mock_offset += n;
    return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    (void)c; return mock_offset == mock_size;
}

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

static void step_stream(void) {
    assert(queued_count);
    uint8_t index = queued[0]; memmove(queued, queued+1, --queued_count);
    pxa_esp_net_slot_t *slot = &g_net->slots[index];
    assert(slot->state == PXA_ESP_NET_SLOT_QUEUED);
    slot->state = PXA_ESP_NET_SLOT_RUNNING;
    stream_step(g_net, slot);
}
static void test_stream(pxa_net_backend_t *b) {
    pxa_net_request_t r = {0}; void *exchange;
    uint8_t data[512], head[1024];
    r.struct_size = sizeof(r); r.abi_minor = 2; r.method = PXA_NET_METHOD_GET;
    r.url = (pxa_bytes_t){(const uint8_t*)"http://test.invalid",19};
    r.timeout_ms = 1000; r.buffer_bytes = 512; r.response_header_bytes = 1024;
    r.max_body_bytes = 8192;
    for (r.buffer_bytes = 512; r.buffer_bytes <= 4096; r.buffer_bytes *= 8) {
      for (mock_mode = 0; mock_mode < 2; ++mock_mode) {
        size_t total = 0, allocations;
        mock_header_waits = 2;
        assert(b->open_exchange(NULL,&r,&exchange) == PXA_STATUS_OK);
        allocations = allocation_count;
        assert(b->exchange_io(NULL,exchange,PXA_IO_READ,data,sizeof(data)) == PXA_STATUS_WOULD_BLOCK);
        for(unsigned i=0;i<8 && !(b->exchange_io(NULL,exchange,PXA_IO_POLL,NULL,0)&PXA_READY_HEADERS);++i) step_stream();
        assert(mock_offset == 0); /* headers are published before any body read */
        assert(b->exchange_io(NULL,exchange,PXA_HTTP_IO_RESPONSE,head,sizeof(head)) > 16);
        assert(pxa_read_u16(head) == 200);
        step_stream(); /* fill receive buffer */
        for (unsigned i=0;i<10;++i) step_stream();
        assert(mock_offset == r.buffer_bytes); /* no growth or reads under backpressure */
        for(unsigned steps=0;steps<100;++steps) {
            int32_t n = b->exchange_io(NULL,exchange,PXA_IO_READ,data,sizeof(data));
            if(n == PXA_STATUS_WOULD_BLOCK) { step_stream(); continue; }
            assert(n >= 0);
            if(!n) break;
            for(int32_t i=0;i<n;++i) assert(data[i] == ((total+i)&255));
            total += (unsigned)n;
        }
        assert(total == 8192 && allocations == allocation_count);
        b->close_exchange(NULL,exchange);step_stream();assert(live_allocations == 1 && !queued_count);
      }
    }
    r.buffer_bytes = 512;
    mock_mode=2; r.method=PXA_NET_METHOD_POST; r.upload_bytes=65536;
    assert(b->open_exchange(NULL,&r,&exchange)==PXA_STATUS_OK);
    step_stream();step_stream();
    assert(b->exchange_io(NULL,exchange,PXA_HTTP_IO_RESPONSE,head,sizeof(head))>16);
    assert(pxa_read_u16(head)==413 && mock_uploaded==0);
    b->close_exchange(NULL,exchange);step_stream();
    mock_mode=0;r.upload_bytes=1024;
    assert(b->open_exchange(NULL,&r,&exchange)==PXA_STATUS_OK);
    step_stream();
    for(unsigned total=0;total<1024;) {
        for(unsigned i=0;i<sizeof(data);++i)data[i]=(uint8_t)(total+i);
        size_t count=1024-total; if(count>sizeof(data))count=sizeof(data);
        int32_t n=b->exchange_io(NULL,exchange,PXA_IO_WRITE,data,count);
        if(n==PXA_STATUS_WOULD_BLOCK){step_stream();continue;}
        assert(n>0);total+=(unsigned)n;
        step_stream();
    }
    assert(b->exchange_io(NULL,exchange,PXA_HTTP_IO_FINISH_UPLOAD,NULL,0)==0);
    while(mock_uploaded<1024)step_stream();
    b->close_exchange(NULL,exchange);step_stream();
    mock_mode=3;r.method=PXA_NET_METHOD_GET;r.upload_bytes=0;
    assert(b->open_exchange(NULL,&r,&exchange)==PXA_STATUS_OK);
    for(unsigned i=0;i<3;++i)step_stream();
    assert(b->exchange_io(NULL,exchange,PXA_HTTP_IO_RESPONSE,head,sizeof(head))==PXA_STATUS_PROTOCOL_ERROR);
    b->close_exchange(NULL,exchange);step_stream();
    mock_mode=0;
    assert(b->open_exchange(NULL,&r,&exchange)==PXA_STATUS_OK);
    step_stream();step_stream();step_stream();step_stream();
    pxa_esp_net_reset_requests();step_stream();
    assert(live_allocations==1 && !queued_count);
    assert(b->open_exchange(NULL,&r,&exchange)==PXA_STATUS_OK);
    mock_time+=1000001;step_stream();
    assert(b->exchange_io(NULL,exchange,PXA_IO_READ,data,sizeof(data))==PXA_STATUS_TIMED_OUT);
    b->close_exchange(NULL,exchange);step_stream();
    assert(live_allocations==1 && !queued_count && mock_cleanups==mock_inits);
    assert(worker_notifications && worker_waits && task_count == 1);
}

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
    test_stream(&backend);
    checked_free(g_net); g_net = NULL;
    assert(live_allocations == 0);
    return 0;
}
