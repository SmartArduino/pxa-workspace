#include "pxa/pxa_container_stream.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct pxa_host_container_stream {size_t received;};
static esp_http_client_config_t config;
static int code=200,cleaned,finished,fail_init;
static esp_err_t network_result=ESP_OK;
static pxa_status_t feed_status=PXA_STATUS_OK,finish_status=PXA_STATUS_OK;
static wifi_ps_type_t power=WIFI_PS_MAX_MODEM;
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *value) {*value=power;return ESP_OK;}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t value) {power=value;return ESP_OK;}
int64_t esp_timer_get_time(void) {static int64_t now;return ++now;}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *value) {
    config=*value;assert(config.disable_auto_redirect && config.timeout_ms>0);
    return fail_init?NULL:&config;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
    assert(client==&config);
    assert(power==WIFI_PS_NONE);
    esp_http_client_event_t event={.user_data=config.user_data,.event_id=HTTP_EVENT_ON_DATA,
        .data="body",.data_len=4};
    /* Some transports keep delivering queued events after a sink error. */
    (void)config.event_handler(&event);(void)config.event_handler(&event);
    return network_result;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) {(void)client;return code;}
void esp_http_client_cleanup(esp_http_client_handle_t client) {(void)client;++cleaned;}
pxa_status_t pxa_host_container_stream_feed(pxa_host_container_stream_t *stream,const uint8_t *data,size_t size) {
    assert(!memcmp(data,"body",size));stream->received+=size;return feed_status;
}
pxa_status_t pxa_host_container_stream_finish(pxa_host_container_stream_t *stream) {
    assert(cleaned && stream->received==8);++finished;return finish_status;
}
int main(void) {
    struct pxa_host_container_stream stream={0};
    assert(pxa_host_container_stream_fetch(&stream,"https://host/file")==PXA_STATUS_INVALID_ARGUMENT);
    assert(!cleaned && !finished);
    assert(!pxa_host_container_stream_fetch(&stream,"http://host/file") && cleaned==1 && finished==1);
    stream.received=0;finish_status=PXA_STATUS_DENIED;
    assert(pxa_host_container_stream_fetch(&stream,"http://host/file")==PXA_STATUS_DENIED && finished==2);
    stream.received=0;feed_status=PXA_STATUS_DENIED;
    assert(pxa_host_container_stream_fetch(&stream,"http://host/file")==PXA_STATUS_DENIED && stream.received==4 && finished==2);
    feed_status=PXA_STATUS_OK;code=302;
    assert(pxa_host_container_stream_fetch(&stream,"http://host/file")==PXA_STATUS_IO_ERROR && finished==2);
    code=200;network_result=ESP_ERR_TIMEOUT;
    assert(pxa_host_container_stream_fetch(&stream,"http://host/file")==PXA_STATUS_IO_ERROR && cleaned==5 && finished==2);
    assert(power==WIFI_PS_MAX_MODEM);
    fail_init=1;
    assert(pxa_host_container_stream_fetch(&stream,"http://host/file")==PXA_STATUS_RESOURCE_LIMIT && cleaned==5);
    puts("container fetch tests passed");return 0;
}
