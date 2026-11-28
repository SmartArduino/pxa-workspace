/* Incremental HTTP exchanges run on the existing network worker. A full
 * receive queue or empty upload queue yields this slot instead of blocking
 * the worker. The worker is the only owner of esp_http_client and storage. */
#include "pxa/http_stream.h"
#include "pxa_esp_wifi_transfer.h"
#include <errno.h>
#include <fcntl.h>

#define PXA_ESP_HTTP_STEP_BYTES 4096u

/* Older ESP-IDF clients do not expose the connected socket. */
extern int esp_http_client_get_socket(esp_http_client_handle_t) __attribute__((weak));

static void stream_bound_tls_read(esp_http_client_handle_t client) {
    if (!esp_http_client_get_socket) return;
    const int fd = esp_http_client_get_socket(client);
    if (fd < 0) return;
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && !(flags & O_NONBLOCK)) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    pxa_http_stream_t stream;
    esp_http_client_handle_t client;
    uint64_t declared_length;
    uint16_t response_status;
    uint8_t phase;
    uint8_t transfer_encoding;
    bool wifi_lease;
#if PXA_ESP_HTTP_REUSE
    uint16_t origin_bytes;
    uint8_t range_headers;
    bool private_response, received_header, reused, retried;
#endif
} pxa_esp_http_t;

static pxa_esp_http_t *http_stream(pxa_esp_net_slot_t *slot) {
    return (pxa_esp_http_t *)slot->body;
}
#if PXA_ESP_HTTP_REUSE
#include "pxa_esp_http_reuse.inc"
#else
static void http_release_client(pxa_esp_net_state_t *net, const pxa_esp_net_slot_t *slot,
                                pxa_esp_http_t *h, pxa_status_t error) {
    (void)net; (void)slot; (void)error;
    if (h->client) esp_http_client_cleanup(h->client);
    h->client = NULL;
}
#endif
static esp_err_t stream_http_event(esp_http_client_event_t *event) {
    pxa_esp_net_slot_t *slot = event->user_data;
#if PXA_ESP_HTTP_REUSE
    if (!slot) return ESP_OK; /* Idle clients have no exchange callback target. */
#endif
    pxa_esp_http_t *h = http_stream(slot);
    if (pxa_http_stream_state(&h->stream) & PXA_HTTP_STREAM_CLOSED)
        return ESP_FAIL;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key ||
        !event->header_value)
        return ESP_OK;
    if (pxa_http_stream_state(&h->stream) & PXA_HTTP_STREAM_HEADERS)
        return ESP_OK;
#if PXA_ESP_HTTP_REUSE
    if (h->range_headers) {
        h->received_header = true;
        if (http_private_header(event->header_key, event->header_value)) h->private_response = true;
    }
#endif
    if (!strcasecmp(event->header_key, "content-length")) {
        const char *p = event->header_value;
        uint64_t length = 0;
        if (!*p) {
            h->stream.error = PXA_STATUS_PROTOCOL_ERROR;
            return ESP_FAIL;
        }
        for (; *p; ++p) {
            if (*p < '0' || *p > '9' ||
                length > (UINT64_MAX - (unsigned)(*p - '0')) / 10u) {
                h->stream.error = PXA_STATUS_PROTOCOL_ERROR;
                return ESP_FAIL;
            }
            length = length * 10u + (unsigned)(*p - '0');
        }
        if (h->declared_length != UINT64_MAX && h->declared_length != length) {
            h->stream.error = PXA_STATUS_PROTOCOL_ERROR;
            return ESP_FAIL;
        }
        h->declared_length = length;
    }
    if (!strcasecmp(event->header_key, "transfer-encoding"))
        h->transfer_encoding = 1;
    if (h->transfer_encoding && h->declared_length != UINT64_MAX) {
        h->stream.error = PXA_STATUS_PROTOCOL_ERROR;
        return ESP_FAIL;
    }
    if (slot->wanted_header_count) {
        uint16_t i;
        for (i = 0; i < slot->wanted_header_count; ++i)
            if (header_name_equal(event->header_key, slot->wanted_headers[i]))
                break;
        if (i == slot->wanted_header_count)
            return ESP_OK;
    }
    if (!pxa_http_stream_header(
            &h->stream, (const uint8_t *)event->header_key, strlen(event->header_key),
            (const uint8_t *)event->header_value, strlen(event->header_value))) {
        pxa_http_stream_limit(&h->stream, PXA_HTTP_LIMIT_HEADERS);
        h->stream.error = PXA_STATUS_LIMIT_EXCEEDED;
        return ESP_FAIL;
    }
    return ESP_OK;
}
static void stream_notify(pxa_esp_net_state_t *net) {
    if (net->notify)
        net->notify(net->notify_context);
}
static void stream_finish(pxa_esp_net_state_t *net, pxa_esp_net_slot_t *slot,
                          pxa_status_t error) {
    pxa_esp_http_t *h = http_stream(slot);
    if(error)ESP_LOGW(PXA_ESP_NET_TAG,"HTTP failure memory: dma_free=%u dma_largest=%u internal_free=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    http_release_client(net, slot, h, error);
    if (h->wifi_lease) {
        pxa_esp_wifi_transfer_release();
        h->wifi_lease = false;
    }
    pxa_http_stream_done(&h->stream, error);
    ESP_LOGI(PXA_ESP_NET_TAG, "HTTP stream completed: status=%d bytes=%llu", (int)error,
             (unsigned long long)h->stream.received_bytes);
    portENTER_CRITICAL(&g_net_lock);
    ++net->metrics.completed_requests;
    net->metrics.response_bytes += h->stream.received_bytes;
    if (h->stream.received_bytes > net->metrics.peak_response_bytes)
        net->metrics.peak_response_bytes =
            (uint32_t)(h->stream.received_bytes > UINT32_MAX
                           ? UINT32_MAX
                           : h->stream.received_bytes);
    portEXIT_CRITICAL(&g_net_lock);
    stream_notify(net);
}
static void stream_schedule(pxa_esp_net_state_t *net, pxa_esp_net_slot_t *slot) {
    uint8_t index = (uint8_t)(slot - net->slots);
    int queue;
    portENTER_CRITICAL(&g_net_lock);
    queue =
        !(pxa_http_stream_state(&http_stream(slot)->stream) & PXA_HTTP_STREAM_DONE) ||
        (pxa_http_stream_state(&http_stream(slot)->stream) & PXA_HTTP_STREAM_CLOSED);
    slot->state = queue ? PXA_ESP_NET_SLOT_QUEUED : PXA_ESP_NET_SLOT_COMPLETE;
    portEXIT_CRITICAL(&g_net_lock);
    if (queue)
        (void)xQueueSendToBack(net->queue, &index, 0);
}
static void stream_step(pxa_esp_net_state_t *net, pxa_esp_net_slot_t *slot) {
    pxa_esp_http_t *h = http_stream(slot);
    pxa_http_stream_t *s = &h->stream;
    uint8_t *block;
    size_t available;
    int amount, blocked = 0;
    if ((pxa_http_stream_state(s) & PXA_HTTP_STREAM_CLOSED) || slot->cancelled) {
        void *storage;
        net->blocked_stream_steps = 0;
        http_release_client(net, slot, h, PXA_STATUS_CANCELLED);
        if (h->wifi_lease) pxa_esp_wifi_transfer_release();
        portENTER_CRITICAL(&g_net_lock);
        if (!(pxa_http_stream_state(s) & PXA_HTTP_STREAM_DONE))
            ++net->metrics.cancelled_requests;
        storage = reset_slot_metadata(slot);
        portEXIT_CRITICAL(&g_net_lock);
        free(storage);
        stream_notify(net);
        return;
    }
    if ((uint64_t)esp_timer_get_time() >= slot->deadline_us) {
        stream_finish(net, slot, PXA_STATUS_TIMED_OUT);
        goto schedule;
    }
    if (h->phase == 0) {
        for (uint16_t i = 0; i < slot->request_header_count; ++i) {
            if (!strcasecmp((const char *)slot->request_headers[i].name.data, "Range")) {
                if (!h->wifi_lease) h->wifi_lease = pxa_esp_wifi_transfer_acquire();
                break;
            }
        }
        esp_http_client_config_t config = {0};
        uint16_t i;
        esp_err_t error = ESP_OK;
        config.url = slot->url;
        config.method = esp_method(slot->method);
        config.timeout_ms = (int)(slot->timeout_ms > 60000u ? 15000u : slot->timeout_ms);
        config.disable_auto_redirect = true;
        config.buffer_size = 512;
#if (CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31) && CONFIG_SPIRAM
        /* The IDF client splits each read at buffer_size. A large download
         * can parse one worker chunk per transport read on this target. */
        if (s->receive.capacity >= PXA_ESP_HTTP_STEP_BYTES) {
            if (s->max_body_bytes >= 128 * 1024)
                config.buffer_size = PXA_ESP_HTTP_STEP_BYTES;
            else if (s->max_body_bytes >= 64 * 1024)
                config.buffer_size = PXA_ESP_HTTP_STEP_BYTES / 2;
        }
#endif
        config.buffer_size_tx = http_tx_buffer_bytes(slot);
        config.event_handler = stream_http_event;
        config.user_data = slot;
        config.crt_bundle_attach = esp_crt_bundle_attach;
#if PXA_ESP_HTTP_REUSE
        const int ordinary_tx = config.buffer_size_tx;
        http_reuse_prepare(slot, h, &config);
        h->client = http_idle_take(net, slot, h, config.timeout_ms);
#endif
        if (!h->client) h->client = esp_http_client_init(&config);
#if (CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31) && CONFIG_SPIRAM
        if (!h->client && config.buffer_size > 512) {
            config.buffer_size = 512;
#if PXA_ESP_HTTP_REUSE
            config.buffer_size_tx = ordinary_tx;
            h->range_headers = 0;
#endif
            h->client = esp_http_client_init(&config);
        }
#endif
        /* Keep signed URLs and all request credentials out of the log. */
#if PXA_ESP_HTTP_REUSE
        ESP_LOGI(PXA_ESP_NET_TAG, "HTTP stream %s started%s", method_name(slot->method),
                 h->reused ? " (reused)" : "");
#else
        ESP_LOGI(PXA_ESP_NET_TAG, "HTTP stream %s started", method_name(slot->method));
#endif
        if (!h->client) {
            stream_finish(net, slot, PXA_STATUS_RESOURCE_LIMIT);
            goto schedule;
        }
#if PXA_ESP_HTTP_REUSE
        ++net->http_clients;
#endif
        for (i = 0; i < slot->request_header_count; ++i) {
            error = esp_http_client_set_header(
                h->client, (const char *)slot->request_headers[i].name.data,
                (const char *)slot->request_headers[i].value.data);
            if (error != ESP_OK)
                break;
        }
        if (error == ESP_OK)
            error = set_store_host_profile(h->client, slot->url);
        if (error == ESP_OK) {
            error = esp_http_client_open(h->client, (int)s->upload_bytes);
#if PXA_ESP_HTTP_REUSE
            if (error != ESP_OK && http_retry_idle(net, slot, h)) goto schedule;
#endif
        }
        if (error != ESP_OK) {
            stream_finish(net, slot,
                          error == ESP_ERR_TIMEOUT ? PXA_STATUS_TIMED_OUT
                                                   : PXA_STATUS_IO_ERROR);
            goto schedule;
        }
        /* open() has finished the handshake and sent the request. A short
         * transport poll can see part of a TLS record. The subsequent TLS
         * read must also yield so other exchanges and cancellation can run. */
        if (slot->method == PXA_NET_METHOD_GET && !s->upload_bytes &&
            strncasecmp(slot->url, "https://", 8) == 0) stream_bound_tls_read(h->client);
        (void)esp_http_client_set_timeout_ms(h->client, 50);
        h->phase = 1;
    } else if (h->phase == 1) {
        available = pxa_http_ring_peek(&s->send, &block, 0);
        if (available) {
            if (available > PXA_ESP_HTTP_STEP_BYTES)
                available = PXA_ESP_HTTP_STEP_BYTES;
            errno = 0;
            amount =
                esp_http_client_write(h->client, (const char *)block, (int)available);
            if (amount > 0 && (size_t)amount <= available) {
                pxa_http_ring_advance(&s->send, (size_t)amount, 0);
                s->sent_bytes += (unsigned)amount;
                stream_notify(net);
            } else if (amount < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                /* An early server rejection may arrive before upload ends. */
                h->phase = 2;
            } else
                blocked = 1;
        } else if ((pxa_http_stream_state(s) & PXA_HTTP_STREAM_UPLOAD_DONE) &&
                   s->sent_bytes == s->upload_bytes)
            h->phase = 2;
        else {
            /* A final rejection can arrive while the app is still producing
             * upload bytes. Check the socket briefly rather than requiring
             * the whole body to be supplied first. */
            (void)esp_http_client_set_timeout_ms(h->client, 1);
            goto fetch_headers;
        }
    } else if (h->phase == 2) {
    fetch_headers:;
        int64_t length = esp_http_client_fetch_headers(h->client);
        (void)esp_http_client_set_timeout_ms(h->client, 50);
        amount = esp_http_client_get_status_code(h->client);
        if (amount >= 100 && amount <= 599)
            h->response_status = (uint16_t)amount;
        if (s->error) {
            stream_finish(net, slot, s->error);
            goto schedule;
        }
        if (length == -ESP_ERR_HTTP_EAGAIN)
            blocked = 1;
        else if (length < 0 || h->response_status < 200 || h->response_status > 599) {
#if PXA_ESP_HTTP_REUSE
            if (length < 0 && http_retry_idle(net, slot, h)) goto schedule;
#endif
            stream_finish(net, slot, PXA_STATUS_PROTOCOL_ERROR);
            goto schedule;
        } else {
            uint64_t body_length = h->declared_length;
            if (slot->method == PXA_NET_METHOD_HEAD || h->response_status == 204 ||
                h->response_status == 304)
                body_length = 0;
            if (body_length != UINT64_MAX && body_length > s->max_body_bytes) {
                pxa_http_stream_limit(s, PXA_HTTP_LIMIT_BODY);
                stream_finish(net, slot, PXA_STATUS_LIMIT_EXCEEDED);
                goto schedule;
            }
            pxa_http_stream_headers(s, h->response_status, body_length);
            h->phase = 3;
            stream_notify(net);
            if (body_length == 0) {
                stream_finish(net, slot, PXA_STATUS_OK);
                goto schedule;
            }
        }
    } else {
        available = pxa_http_ring_peek(&s->receive, &block, 1);
        if (!available)
            blocked = 1;
        else {
            uint8_t extra;
            /* Bound the work on one exchange before serving the next. */
            if (available > PXA_ESP_HTTP_STEP_BYTES)
                available = PXA_ESP_HTTP_STEP_BYTES;
            if (s->received_bytes == s->max_body_bytes) {
                block = &extra;
                available = 1;
            } else if (available > s->max_body_bytes - s->received_bytes)
                available = (size_t)(s->max_body_bytes - s->received_bytes);
            amount = esp_http_client_read(h->client, (char *)block, (int)available);
            if (amount > 0 && (size_t)amount <= available) {
                if (s->received_bytes == s->max_body_bytes) {
                    pxa_http_stream_limit(s, PXA_HTTP_LIMIT_BODY);
                    stream_finish(net, slot, PXA_STATUS_LIMIT_EXCEEDED);
                    goto schedule;
                }
                pxa_http_ring_advance(&s->receive, (size_t)amount, 1);
                s->received_bytes += (unsigned)amount;
                stream_notify(net);
                if (s->content_length != UINT64_MAX &&
                    s->received_bytes == s->content_length) {
                    stream_finish(net, slot, PXA_STATUS_OK);
                    goto schedule;
                }
            } else if (amount == -ESP_ERR_HTTP_EAGAIN)
                blocked = 1;
            else if (amount == 0 &&
                     esp_http_client_is_complete_data_received(h->client)) {
                stream_finish(net, slot, PXA_STATUS_OK);
                goto schedule;
            } else {
                stream_finish(net, slot, PXA_STATUS_IO_ERROR);
                goto schedule;
            }
        }
    }
schedule:
    if (blocked) {
        /* Give the other queued exchanges a turn before sleeping. Once an
         * entire pass is blocked, wait for app progress or a socket retry. */
        if (net->blocked_stream_steps < uxQueueMessagesWaiting(net->queue)) {
            ++net->blocked_stream_steps;
        } else {
            net->blocked_stream_steps = 0;
            (void)ulTaskNotifyTake(pdTRUE, 1);
        }
    } else {
        net->blocked_stream_steps = 0;
    }
    stream_schedule(net, slot);
}
