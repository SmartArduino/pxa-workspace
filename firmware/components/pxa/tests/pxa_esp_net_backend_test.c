#if PXA_ESP_HTTP_REUSE
#define PXA_TEST_HEAP_SNAPSHOT 1
#define CONFIG_PXA_RESOURCE_PSRAM_HEADROOM_BYTES 0
#endif
#include "pxa_test_platform.h"
#include <setjmp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <esp_wifi.h>
#include "pxa_esp_wifi_transfer.h"
#include <string.h>

static unsigned critical_depth, task_count, notifications;
static unsigned worker_notifications, worker_pending_notifications, worker_waits;
static size_t allocation_count, live_allocations;
static int fail_allocation, fail_queue_send, cancel_in_transport, fail_transport;
static int skip_platform_transport, fail_profile_header;
static char mock_profile[101];
static unsigned mock_profile_headers;
static uint8_t queued[4];
static unsigned queued_count;
static jmp_buf worker_idle;
static esp_http_client_config_t mock_client;
static int mock_socket_fd = -1;
static unsigned mock_offset, mock_size, mock_uploaded, mock_cleanups, mock_header_waits, mock_inits;
static unsigned mock_mode;
#if PXA_ESP_HTTP_REUSE
static unsigned reused_opens, stale_open, stale_head, reject_update, fail_after_header;
static bool persistent = true, low_heap, reset_on_alloc;
typedef struct {
    esp_http_client_config_t config;
    unsigned offset;
    bool used;
} range_mock_t;
static range_mock_t range_clients[4];
static range_mock_t *range_client(esp_http_client_handle_t handle) {
    for (unsigned i = 0; i < 4; ++i)
        if (handle == &range_clients[i]) return &range_clients[i];
    return NULL;
}
static const char *response_header;
static unsigned range_status = 206;
size_t heap_caps_get_total_size(unsigned caps) { (void)caps; return 8u << 20; }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return low_heap ? 4096 : 8u << 20; }
size_t heap_caps_get_largest_free_block(unsigned caps) { return heap_caps_get_free_size(caps); }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url) {
    range_mock_t *c = range_client(client);
    assert(c && c->used && !c->config.user_data && url);
    if (reject_update) { --reject_update; return ESP_FAIL; }
    ++reused_opens; c->offset = 0;
    return ESP_OK;
}
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data) {
    range_mock_t *c = range_client(client);
    assert(c && c->used); c->config.user_data = data; return ESP_OK;
}
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t client) {
    return client && persistent;
}
#endif
static int expected_rx_buffer = 512, reject_rx_buffer;
static int mock_status;
static int64_t mock_time = 1000;

static wifi_ps_type_t wifi_power = WIFI_PS_MIN_MODEM;
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *power) { *power = wifi_power; return ESP_OK; }
esp_err_t esp_wifi_set_ps(wifi_ps_type_t power) { wifi_power = power; return ESP_OK; }

static void checked_free(void *memory) {
    assert(critical_depth == 0);
    if (memory != NULL) { assert(live_allocations != 0); --live_allocations; }
    free(memory);
}

static void *checked_calloc(size_t count, size_t bytes) {
    void *memory = calloc(count, bytes);
    if (memory) { ++allocation_count; ++live_allocations; }
    return memory;
}

#define ESP_PLATFORM 1
#define free checked_free
#define calloc checked_calloc
#include "../src/services/pxa_esp_net.c"
#undef free
#undef calloc

/* HTTP transport fixture: the optional LAN provider has its own socket tests. */
int pxa_lan_native_create(const pxa_lan_native_config_t *config, pxa_lan_backend_t *backend) {
    (void)config; (void)backend;
    return 0;
}

int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
    void *context, unsigned priority, TaskHandle_t *task) {
    (void)fn; (void)name; (void)stack; (void)context; (void)priority; (void)task;
    return 0; // Exercise native socket worker allocation failure and rollback.
}
esp_transport_handle_t esp_transport_tcp_init(void) { return NULL; }
esp_transport_handle_t esp_transport_ssl_init(void) { return NULL; }
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t,void (*attach)(void)) {(void)t;(void)attach;}
int esp_transport_connect(esp_transport_handle_t t,const char *h,int p,int ms) {(void)t;(void)h;(void)p;(void)ms;return -1;}
int esp_transport_write(esp_transport_handle_t t,const char *b,int n,int ms) {(void)t;(void)b;(void)n;(void)ms;return -1;}
int esp_transport_poll_read(esp_transport_handle_t t,int ms) {(void)t;(void)ms;return -1;}
int esp_transport_read(esp_transport_handle_t t,char *b,int n,int ms) {(void)t;(void)b;(void)n;(void)ms;return -1;}
int esp_transport_close(esp_transport_handle_t t) {(void)t;return 0;}
int esp_transport_destroy(esp_transport_handle_t t) {(void)t;return 0;}

static esp_netif_t mock_netif;
esp_netif_t *esp_netif_get_default_netif(void) { return &mock_netif; }
bool esp_netif_is_netif_up(esp_netif_t *netif) { return netif->up != 0; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out) {
    out->ip.addr = netif->address; return ESP_OK;
}

void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void test_enter(void) { assert(critical_depth++ == 0); }
void test_leave(void) { assert(critical_depth-- == 1); }
void *heap_caps_malloc(size_t size, unsigned caps) {
    void *memory;
    (void)caps;
    assert(critical_depth == 0);
#if PXA_ESP_HTTP_REUSE
    if (reset_on_alloc) {
        reset_on_alloc = false;
        pxa_esp_net_reset_requests();
    }
#endif
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
unsigned uxQueueMessagesWaiting(QueueHandle_t queue) {
    assert(queue == queued);
    return queued_count;
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
void vQueueDeleteWithCaps(QueueHandle_t queue) { (void)queue; }
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack,
    void *context, unsigned priority, TaskHandle_t *task, unsigned caps) {
    (void)fn; (void)name; (void)stack; (void)priority; (void)caps;
    ++task_count; *task = context; return pdPASS;
}
int64_t esp_timer_get_time(void) { return mock_time; }
void esp_crt_bundle_attach(void) {}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    assert(critical_depth == 0);
    if (reject_rx_buffer && c->buffer_size == reject_rx_buffer) {
        reject_rx_buffer = 0;
        return NULL;
    }
    ++mock_inits; mock_client = *c; mock_offset = mock_uploaded = 0;
    mock_profile[0] = '\0'; mock_profile_headers = 0;
    mock_size = mock_mode == 2 ? 0 : 8192;
    mock_status = mock_mode == 2 ? 413 : 200;
#if PXA_ESP_HTTP_REUSE
    if (mock_mode == 4) mock_status = (int)range_status;
#endif
    assert(c->disable_auto_redirect && c->buffer_size == expected_rx_buffer);
#if PXA_ESP_HTTP_REUSE
    if (mock_mode == 4) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!range_clients[i].used) {
                range_clients[i] = (range_mock_t){*c, 0, true};
                return &range_clients[i];
            }
        }
        assert(0 && "a fifth live HTTP client");
    }
#endif
    return &mock_client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *n, const char *v) {
    (void)c;
    if (!strcasecmp(n, PXA_STORE_HOST_PROFILE_HEADER)) {
        ++mock_profile_headers;
        if (fail_profile_header) return ESP_ERR_NO_MEM;
        assert(strlen(v) < sizeof(mock_profile));
        strcpy(mock_profile, v);
    }
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *b, int n) {
    (void)c; (void)b; (void)n; return ESP_FAIL;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { (void)c; return ESP_FAIL; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return mock_status; }
void esp_http_client_cleanup(esp_http_client_handle_t c) {
    assert(!critical_depth);
#if PXA_ESP_HTTP_REUSE
    range_mock_t *r = range_client(c);
    if (r) { assert(r->used); r->used = false; }
#endif
    (void)c; ++mock_cleanups;
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
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
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) {
    (void)c; (void)n;
#if PXA_ESP_HTTP_REUSE
    if (stale_open) { --stale_open; return ESP_FAIL; }
#endif
    return ESP_OK;
}
int esp_http_client_get_socket(esp_http_client_handle_t c) { (void)c; return mock_socket_fd; }
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
#if PXA_ESP_HTTP_REUSE
    if (mock_mode == 4) {
        if (stale_head) { --stale_head; return -1; }
        range_mock_t *r = range_client(c);
        assert(r && r->used && r->config.user_data);
        esp_http_client_event_t event = {.user_data = r->config.user_data,
            .event_id = HTTP_EVENT_ON_HEADER, .header_key = "Content-Length", .header_value = "8192"};
        r->config.event_handler(&event);
        if (response_header) {
            event.header_key = (char *)response_header; event.header_value = "private";
            r->config.event_handler(&event);
        }
        if (fail_after_header) { --fail_after_header; return -1; }
        return 8192;
    }
#endif
    if (mock_mode != 1) mock_header("Content-Length", mock_size ? "8192" : "0");
    else mock_header("Transfer-Encoding", "chunked");
    mock_header("Set-Cookie", "a=1"); mock_header("Set-Cookie", "b=2");
    if (mock_mode == 3) mock_header("Content-Length", "8193");
    return mock_size;
}
int esp_http_client_read(esp_http_client_handle_t c, char *s, int n) {
    (void)c;
#if PXA_ESP_HTTP_REUSE
    range_mock_t *r = range_client(c);
    if (r) {
        assert(r->used && r->offset <= 8192);
        if ((unsigned)n > 8192 - r->offset) n = 8192 - r->offset;
        for (int i = 0; i < n; ++i) s[i] = (char)((r->offset + i) & 255);
        r->offset += n; return n;
    }
#endif
    if ((unsigned)n > mock_size - mock_offset) n = (int)(mock_size - mock_offset);
    for (int i = 0; i < n; ++i) s[i] = (char)((mock_offset + i) & 255);
    mock_offset += n;
    return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
#if PXA_ESP_HTTP_REUSE
    range_mock_t *r = range_client(c);
    if (r) return r->used && r->offset == 8192;
#endif
    (void)c; return mock_offset == mock_size;
}

int pxa_platform_net_http_request(const pxa_net_request_t *request,
    uint8_t *body, size_t capacity, size_t *size, uint16_t *status,
    char *content_type, size_t content_capacity, pxa_net_header_t *headers,
    size_t header_capacity, size_t *header_count, uint8_t *header_storage,
    size_t header_storage_capacity, size_t *header_bytes) {
    (void)headers; (void)header_capacity;
    if (skip_platform_transport) return PXA_PLATFORM_NET_FETCH_NOT_USED;
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

static void test_store_profile(pxa_net_backend_t *backend) {
    const char *accepted[] = {
        "https://app.doit.am/api/v2/catalog",
        "https://APP.DOIT.AM:443/api/v2/catalog?profile=old",
        "https://app.doit.am/api/v2/apps/pxa-store?profile=old",
        "https://app.doit.am/api/v2/device/update-check",
    };
    const char *rejected[] = {
        "", "https://app.doit.am", "http://app.doit.am/api/v2/catalog",
        "https://app.doit.am.evil.invalid/api/v2/catalog",
        "https://app.doit.am@evil.invalid/api/v2/catalog",
        "https://evil.invalid@ app.doit.am/api/v2/catalog",
        "https://app.doit.am:8443/api/v2/catalog",
        "https://app.doit.am/api/v2/catalogue",
        "https://app.doit.am/api/v2/catalog/anything",
        "https://app.doit.am/api/v2/device/update-check-other",
        "https://app.doit.am/api/v2/apps/",
        "https://app.doit.am/api/v2/apps/?id=pxa-store",
        "https://app.doit.am/api/v2/artifacts/1/download-ticket",
        "https://downloads.doit.am/api/v2/catalog",
    };
    assert(!pxa_store_host_profile_url(NULL));
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i)
        assert(pxa_store_host_profile_url(accepted[i]));
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i)
        assert(!pxa_store_host_profile_url(rejected[i]));
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    const char *expected = "esp32-s3-pxa-0.11.2";
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
    const char *expected = "esp32-s31-pxa-0.11.2";
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
    const char *expected = "esp32-p4-pxa-0.11.2";
#else
    const char *expected = "";
#endif
    pxa_net_request_t request = {0};
    request.struct_size = sizeof(request);
    request.method = PXA_NET_METHOD_GET;
    request.timeout_ms = 1000;
    request.max_response_bytes = 64;
    request.buffer_bytes = 512;
    request.response_header_bytes = 1024;
    request.max_body_bytes = 8192;
    skip_platform_transport = 1;
    for (unsigned streaming = 0; streaming < 2; ++streaming) {
        request.abi_minor = streaming ? 2 : 1;
        for (unsigned official = 0; official < 2; ++official) {
            const char *url = official ? accepted[0] : rejected[3];
            request.url = (pxa_bytes_t){(const uint8_t *)url, strlen(url)};
            if (streaming) {
                void *exchange;
                assert(backend->open_exchange(NULL, &request, &exchange) == PXA_STATUS_OK);
                step_stream();
                backend->close_exchange(NULL, exchange);
                step_stream();
            } else {
                uint64_t operation;
                pxa_net_response_t response;
                assert(backend->start(NULL, &request, &operation) == PXA_STATUS_OK);
                drain_worker();
                assert(backend->poll(NULL, operation, &response) == PXA_STATUS_UNAVAILABLE);
            }
            assert(strcmp(mock_profile, official ? expected : "") == 0);
            assert(mock_profile_headers == (unsigned)(official && *expected));
            assert(live_allocations == 1 && !queued_count);
        }
    }
    if (*expected) {
        /* Host replaces even a differently cased guest declaration. */
        pxa_net_header_t header = {
            {(const uint8_t *)"x-pxa-host-profile", 18},
            {(const uint8_t *)"stale", 5},
        };
        void *exchange;
        request.headers = &header; request.header_count = 1;
        assert(backend->open_exchange(NULL, &request, &exchange) == PXA_STATUS_OK);
        step_stream();
        assert(strcmp(mock_profile, expected) == 0 && mock_profile_headers == 2);
        backend->close_exchange(NULL, exchange); step_stream();
        request.headers = NULL; request.header_count = 0;
        fail_profile_header = 1;
        assert(backend->open_exchange(NULL, &request, &exchange) == PXA_STATUS_OK);
        step_stream();
        assert(http_stream(exchange)->stream.error == PXA_STATUS_IO_ERROR);
        backend->close_exchange(NULL, exchange); step_stream();
        fail_profile_header = 0;
    }
    skip_platform_transport = 0;
    assert(live_allocations == 1 && !queued_count && mock_cleanups == mock_inits);
}
static void test_stream(pxa_net_backend_t *b) {
    pxa_net_request_t r = {0}; void *exchange;
    uint8_t data[512], head[1024];
    r.struct_size = sizeof(r); r.abi_minor = 2; r.method = PXA_NET_METHOD_GET;
    r.url = (pxa_bytes_t){(const uint8_t*)"http://test.invalid",19};
    r.timeout_ms = 1000; r.buffer_bytes = 512; r.response_header_bytes = 1024;
    r.max_body_bytes = 8192;
#if (CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31) && CONFIG_SPIRAM
    {
        const struct { uint32_t body, ring; int rx, reject; } cases[] = {
            {65535, 4096, 512, 0}, {65536, 4096, 2048, 0},
            {124362, 4096, 2048, 0}, {131072, 4096, 4096, 0},
            {124362, 512, 512, 0}, {124362, 4096, 512, 2048}
        };
        for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
            r.max_body_bytes = cases[i].body;
            r.buffer_bytes = cases[i].ring;
            expected_rx_buffer = cases[i].rx;
            reject_rx_buffer = cases[i].reject;
            assert(b->open_exchange(NULL, &r, &exchange) == PXA_STATUS_OK);
            step_stream();
            assert(!reject_rx_buffer);
            b->close_exchange(NULL, exchange);
            step_stream();
            assert(live_allocations == 1 && !queued_count);
        }
        r.max_body_bytes = 8192;
        r.buffer_bytes = expected_rx_buffer = 512;
    }
#endif
    /* A live exchange survives 65 seconds and still releases its bounded
     * buffer on cancellation. Connection setup keeps a short timeout. */
    r.timeout_ms = PXA_NET_MAX_STREAM_TIMEOUT_MS;
    int pair[2];
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    mock_socket_fd=pair[0];
    r.url=(pxa_bytes_t){(const uint8_t*)"https://test.invalid",20};
    assert(b->open_exchange(NULL,&r,&exchange) == PXA_STATUS_OK);
    step_stream();
    assert(fcntl(mock_socket_fd,F_GETFL) & O_NONBLOCK);
    close(pair[0]); close(pair[1]); mock_socket_fd=-1;
    r.url=(pxa_bytes_t){(const uint8_t*)"http://test.invalid",19};
    assert(mock_client.timeout_ms <= 15000);
    mock_time += INT64_C(65000000);
    step_stream();
    assert(http_stream((pxa_esp_net_slot_t *)exchange)->stream.error == 0);
    b->close_exchange(NULL,exchange);step_stream();
    assert(live_allocations == 1 && !queued_count);
    r.timeout_ms = 1000;
    {
        void *second;
        assert(b->open_exchange(NULL, &r, &exchange) == PXA_STATUS_OK);
        for (unsigned i = 0; i < 4; ++i) step_stream();
        assert(!pxa_http_ring_writable(&http_stream(exchange)->stream.receive));
        assert(b->open_exchange(NULL, &r, &second) == PXA_STATUS_OK);
        unsigned waits = worker_waits;
        step_stream(); /* Full first ring, second request ready to open. */
        assert(worker_waits == waits);
        step_stream(); /* Open second. */
        step_stream(); /* First still full. */
        step_stream(); /* GET switches from upload to headers. */
        mock_header_waits = 1;
        step_stream(); /* First full. */
        assert(worker_waits == waits);
        step_stream(); /* Second also blocked. Sleep once for both. */
        assert(worker_waits == waits + 1);
        b->close_exchange(NULL, exchange);
        b->close_exchange(NULL, second);
        step_stream();
        step_stream();
        assert(live_allocations == 1 && !queued_count);
    }
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
    for (unsigned kind = 0; kind < 3; ++kind) {
        mock_mode = kind == 1 ? 1 : 0;
        r.max_body_bytes = kind == 2 ? 8192 : 100;
        r.response_header_bytes = kind == 2 ? 16 : 1024;
        assert(b->open_exchange(NULL,&r,&exchange) == PXA_STATUS_OK);
        assert(b->exchange_io(NULL,exchange,PXA_HTTP_IO_LIMIT_REASON,NULL,0) == PXA_STATUS_WOULD_BLOCK);
        for (unsigned steps = 0; steps < 16 &&
             !(pxa_http_stream_state(&http_stream(exchange)->stream) & PXA_HTTP_STREAM_DONE); ++steps) step_stream();
        assert(http_stream(exchange)->stream.error == PXA_STATUS_LIMIT_EXCEEDED);
        assert(b->exchange_io(NULL,exchange,PXA_HTTP_IO_LIMIT_REASON,NULL,0) ==
               (kind == 2 ? (int32_t)PXA_HTTP_LIMIT_HEADERS : (int32_t)PXA_HTTP_LIMIT_BODY));
        assert(mock_offset == (kind == 1 ? 101u : 0u));
        b->close_exchange(NULL,exchange); step_stream();
        assert(live_allocations == 1 && !queued_count);
    }
    r.max_body_bytes = 8192; r.response_header_bytes = 1024;
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

#if PXA_ESP_HTTP_REUSE
#include "pxa_esp_http_reuse_test.inc"
#endif

int main(void) {
    assert(pxa_esp_wifi_transfer_acquire() && wifi_power == WIFI_PS_NONE);
    assert(pxa_esp_wifi_transfer_acquire());
    pxa_esp_wifi_transfer_release();
    assert(wifi_power == WIFI_PS_NONE);
    pxa_esp_wifi_transfer_release();
    assert(wifi_power == WIFI_PS_MIN_MODEM);
    assert(pxa_esp_lan_backend() == NULL);
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
    uint32_t connectivity = 99;
    assert(backend.connectivity(NULL, &connectivity) == PXA_STATUS_OK && connectivity == PXA_NET_CONNECTIVITY_OFFLINE);
    mock_netif.up = 1;
    assert(backend.connectivity(NULL, &connectivity) == PXA_STATUS_OK && connectivity == PXA_NET_CONNECTIVITY_OFFLINE);
    mock_netif.address = 1;
    assert(backend.connectivity(NULL, &connectivity) == PXA_STATUS_OK && connectivity == PXA_NET_CONNECTIVITY_ONLINE);
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
#if PXA_ESP_HTTP_REUSE
    test_http_connection_reuse(&backend);
#endif
    test_store_profile(&backend);
    pxa_socket_request_t socket_request = {0};
    socket_request.host = (pxa_bytes_t){(const uint8_t *)"test.invalid", 12};
    socket_request.port = 443; socket_request.tls = 1;
    socket_request.buffer_bytes = 512; socket_request.connect_timeout_ms = 100;
    void *socket = (void *)1;
    size_t live_before_socket = live_allocations;
    assert(backend.open_socket(NULL, &socket_request, &socket) == PXA_STATUS_RESOURCE_LIMIT);
    assert(socket == NULL && esp_socket_workers == 0 && live_allocations == live_before_socket);
    checked_free(g_net); g_net = NULL;
    assert(live_allocations == 0);
    return 0;
}
