/* Only the network worker owns the idle IDF client. Each exchange still
 * owns its own body, response headers, permission check and deadline. */
#include "pxa_esp_resource_policy.h"

#define PXA_HTTP_IDLE_US UINT64_C(4000000)
#define PXA_HTTP_RANGE_TX_BYTES 3072

static bool http_reuse_headroom(void) {
    /* Keep the configured native reserve and room for another stream's
     * socket buffers in internal memory. The fresh-client path can still try. */
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 16384 &&
        pxa_esp_resource_psram_available(heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM), CONFIG_PXA_RESOURCE_PSRAM_HEADROOM_BYTES) > 0;
}

static void http_idle_close(pxa_esp_net_state_t *net) {
    if (net->idle.client) {
        esp_http_client_cleanup(net->idle.client);
        net->idle.client = NULL;
    }
}

static TickType_t http_idle_wait(pxa_esp_net_state_t *net) {
    if (!net->idle.client) return portMAX_DELAY;
    uint64_t now = esp_timer_get_time();
    if (now >= net->idle.expires_us || !http_reuse_headroom() ||
        net->idle.generation != __atomic_load_n(&net->generation, __ATOMIC_ACQUIRE)) {
        http_idle_close(net);
        return portMAX_DELAY;
    }
    /* Only wake while a connection is retained. Teardown and heap pressure
     * need no extra queue entry, even when all exchange slots are runnable. */
    unsigned ms = (unsigned)((net->idle.expires_us - now) / 1000) + 1;
    return pdMS_TO_TICKS(ms < 100 ? ms : 100) + 1;
}

static unsigned http_range_headers(const pxa_esp_net_slot_t *slot) {
    static const char *const allowed[] = {"Range", "Accept", "Referer", "User-Agent"};
    unsigned mask = 0;
    for (unsigned i = 0; i < slot->request_header_count; ++i) {
        const pxa_net_header_t *header = &slot->request_headers[i];
        if (!pxa_net_header_value_valid(header->value)) return 0;
        unsigned j = 0;
        while (j < 4 && strcasecmp((const char *)header->name.data, allowed[j])) ++j;
        if (j == 4 || (mask & (1u << j))) return 0;
        if (j == 0 && strncmp((const char *)header->value.data, "bytes=", 6)) return 0;
        mask |= 1u << j;
    }
    return mask & 1u ? mask : 0;
}

static void http_reuse_prepare(const pxa_esp_net_slot_t *slot, pxa_esp_http_t *h,
                               esp_http_client_config_t *config) {
    const pxa_http_stream_t *s = &h->stream;
    if (!slot->component || slot->method != PXA_NET_METHOD_GET || s->upload_bytes ||
        s->max_body_bytes < 16384 || s->receive.capacity < PXA_ESP_HTTP_STEP_BYTES ||
        config->buffer_size_tx > PXA_HTTP_RANGE_TX_BYTES || pxa_store_host_profile_url(slot->url))
        return;
    pxa_bytes_t url = {(const uint8_t *)slot->url, strlen(slot->url)};
    pxa_bytes_t origin;
    if (!pxa_net_parse_https_url(url, &origin) || origin.size >= sizeof(g_net->idle.origin)) return;
    h->range_headers = http_range_headers(slot);
    if (!h->range_headers || !http_reuse_headroom()) {
        h->range_headers = 0;
        return;
    }
    h->origin_bytes = (uint16_t)origin.size;
    config->buffer_size = PXA_ESP_HTTP_STEP_BYTES;
    config->buffer_size_tx = PXA_HTTP_RANGE_TX_BYTES;
}

static esp_http_client_handle_t http_idle_take(pxa_esp_net_state_t *net,
        pxa_esp_net_slot_t *slot, pxa_esp_http_t *h, int timeout) {
    (void)http_idle_wait(net);
    if (!net->idle.client) return NULL;
    esp_http_client_handle_t client = net->idle.client;
    net->idle.client = NULL;
    /* Same header set: the caller immediately overwrites every retained value.
     * Authorization, Cookie, Host, conditional headers and duplicate names never qualify. */
    if (h->range_headers && !h->retried && net->idle.component == slot->component &&
        net->idle.generation == slot->generation && net->idle.headers == h->range_headers &&
        strlen(net->idle.origin) == h->origin_bytes && !memcmp(net->idle.origin, slot->url, h->origin_bytes) &&
        esp_http_client_set_url(client, slot->url) == ESP_OK &&
        esp_http_client_set_timeout_ms(client, timeout) == ESP_OK &&
        esp_http_client_set_user_data(client, slot) == ESP_OK) {
        h->reused = true;
        return client;
    }
    esp_http_client_cleanup(client);
    return NULL;
}

static bool http_private_header(const char *name, const char *value) {
    if (!strcasecmp(name, "Cache-Control")) {
        for (const char *p = value; *p; ++p)
            if (!strncasecmp(p, "private", 7) || !strncasecmp(p, "no-store", 8)) return true;
    }
    return !strcasecmp(name, "Set-Cookie") || !strcasecmp(name, "Set-Cookie2") ||
        !strcasecmp(name, "WWW-Authenticate") || !strcasecmp(name, "Proxy-Authenticate") ||
        !strcasecmp(name, "Authentication-Info") || !strcasecmp(name, "Proxy-Authentication-Info") ||
        !strcasecmp(name, "Location");
}

static void http_release_client(pxa_esp_net_state_t *net, const pxa_esp_net_slot_t *slot,
                                pxa_esp_http_t *h, pxa_status_t error) {
    if (!h->client) return;
    --net->http_clients;
    if (!error && h->range_headers && !net->http_clients && !h->private_response &&
        h->response_status == 206 && h->declared_length == h->stream.received_bytes &&
        slot->generation == __atomic_load_n(&net->generation, __ATOMIC_ACQUIRE) &&
        !(pxa_http_stream_state(&h->stream) & PXA_HTTP_STREAM_CLOSED) &&
        esp_http_client_is_complete_data_received(h->client) &&
        esp_http_client_is_persistent_connection(h->client) && http_reuse_headroom() &&
        esp_http_client_set_user_data(h->client, NULL) == ESP_OK) {
        net->idle.client = h->client;
        net->idle.component = slot->component;
        net->idle.generation = slot->generation;
        net->idle.headers = h->range_headers;
        memcpy(net->idle.origin, slot->url, h->origin_bytes);
        net->idle.origin[h->origin_bytes] = '\0';
        net->idle.expires_us = esp_timer_get_time() + PXA_HTTP_IDLE_US;
    } else {
        esp_http_client_cleanup(h->client);
    }
    h->client = NULL;
}

static bool http_retry_idle(pxa_esp_net_state_t *net, const pxa_esp_net_slot_t *slot,
                            pxa_esp_http_t *h) {
    if (!h->reused || h->retried || h->received_header || h->stream.error ||
        h->stream.received_bytes || (pxa_http_stream_state(&h->stream) &
            (PXA_HTTP_STREAM_HEADERS | PXA_HTTP_STREAM_CLOSED)) ||
        slot->generation != __atomic_load_n(&net->generation, __ATOMIC_ACQUIRE) ||
        (uint64_t)esp_timer_get_time() >= slot->deadline_us) return false;
    http_release_client(net, slot, h, PXA_STATUS_IO_ERROR);
    h->reused = false;
    h->retried = true;
    h->response_status = 0;
    h->phase = 0;
    return true;
}
