#include "pxa/pxa_container_stream.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>
#include <string.h>

typedef struct {
    pxa_host_container_stream_t *stream;
    pxa_status_t status;
    uint64_t bytes,feed_us;
} fetch_sink_t;

static esp_err_t receive_body(esp_http_client_event_t *event) {
    fetch_sink_t *sink=event->user_data;
    if(event->event_id!=HTTP_EVENT_ON_DATA || event->data_len<=0)return ESP_OK;
    if(sink->status==PXA_STATUS_OK) {
        int64_t start=esp_timer_get_time();
        sink->status=pxa_host_container_stream_feed(sink->stream,event->data,(size_t)event->data_len);
        sink->feed_us+=(uint64_t)(esp_timer_get_time()-start);
        sink->bytes+=(size_t)event->data_len;
    }
    return sink->status==PXA_STATUS_OK?ESP_OK:ESP_FAIL;
}

pxa_status_t pxa_host_container_stream_fetch(pxa_host_container_stream_t *stream,const char *url) {
    /* This endpoint is armed over the local development session. The host
     * serves exactly one package under an unguessable, short-lived URL.
     * Signatures and the caller-supplied digest remain mandatory; network
     * bytes never become executable before the normal installation commit. */
    if(!stream || !url || strncmp(url,"http://",7) || !url[7])return PXA_STATUS_INVALID_ARGUMENT;
    fetch_sink_t sink={.stream=stream,.status=PXA_STATUS_OK};
    esp_http_client_config_t config={.url=url,.method=HTTP_METHOD_GET,.timeout_ms=30000,
        .disable_auto_redirect=true,.event_handler=receive_body,.user_data=&sink,.buffer_size=16384};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if(!client)return PXA_STATUS_RESOURCE_LIMIT;
    /* Bulk transfer is a short foreground operation. Restore the previous
     * power policy on failure as well as success; Ethernet/uninitialized Wi-Fi
     * simply skips this optional optimization. */
    wifi_ps_type_t previous=WIFI_PS_NONE,current;
    int restore=esp_wifi_get_ps(&previous)==ESP_OK && previous!=WIFI_PS_NONE &&
        esp_wifi_set_ps(WIFI_PS_NONE)==ESP_OK;
    int64_t start=esp_timer_get_time();
    esp_err_t fetched=esp_http_client_perform(client);
    int code=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if(restore && esp_wifi_get_ps(&current)==ESP_OK && current==WIFI_PS_NONE)
        (void)esp_wifi_set_ps(previous);
    ESP_LOGI("PxaTransfer","HTTP bytes=%" PRIu64 " elapsed_ms=%" PRIu64 " feed_ms=%" PRIu64,
        sink.bytes,(uint64_t)(esp_timer_get_time()-start)/1000,sink.feed_us/1000);
    if(sink.status!=PXA_STATUS_OK)return sink.status;
    if(fetched!=ESP_OK || code!=200)return PXA_STATUS_IO_ERROR;
    return pxa_host_container_stream_finish(stream);
}
