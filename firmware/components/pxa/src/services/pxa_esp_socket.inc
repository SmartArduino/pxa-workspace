/* SPDX-License-Identifier: MIT */
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "pxa/socket_stream.h"
typedef struct {
  pxa_socket_stream_t stream;
  uint32_t references, timeout_ms;
  uint16_t port;
  uint8_t tls;
  pxa_esp_net_notify_fn notify;
  void *notify_context;
  char host[254];
  uint8_t storage[];
} esp_socket_t;
static uint32_t esp_socket_workers;
static void esp_socket_release(esp_socket_t *s) {
  if (__atomic_sub_fetch(&s->references, 1, __ATOMIC_ACQ_REL) == 0)
    free(s);
}
static int esp_socket_closed(esp_socket_t *s) {
  return !!(__atomic_load_n(&s->stream.state, __ATOMIC_ACQUIRE) &
            PXA_SOCKET_CLOSED);
}
static void esp_socket_worker(void *context) {
  esp_socket_t *s = context;
  int32_t error = PXA_STATUS_IO_ERROR;
  esp_transport_handle_t transport =
      s->tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
  if (!transport) {
    error = PXA_STATUS_RESOURCE_LIMIT;
    goto done;
  }
  if (s->tls)
    esp_transport_ssl_crt_bundle_attach(transport, esp_crt_bundle_attach);
  if (esp_socket_closed(s) || esp_transport_connect(transport, s->host, s->port,
                                                    (int)s->timeout_ms) < 0)
    goto done;
  __atomic_fetch_or(&s->stream.state, PXA_SOCKET_CONNECTED, __ATOMIC_RELEASE);
  if (s->notify)
    s->notify(s->notify_context);
  while (!esp_socket_closed(s)) {
    uint8_t *data;
    size_t size = pxa_http_ring_peek(&s->stream.send, &data, 0);
    if (size) {
      int n =
          esp_transport_write(transport, (const char *)data, (int)size, 100);
      if (n < 0)
        goto done;
      if (n > 0) {
        pxa_http_ring_advance(&s->stream.send, (size_t)n, 0);
        if (s->notify)
          s->notify(s->notify_context);
      }
    }
    size = pxa_http_ring_peek(&s->stream.receive, &data, 1);
    if (size) {
      int ready = esp_transport_poll_read(transport, 10);
      if (ready < 0)
        goto done;
      if (ready) {
        int n = esp_transport_read(transport, (char *)data, (int)size, 100);
        if (n < 0)
          goto done;
        if (!n) {
          error = PXA_STATUS_OK;
          goto done;
        }
        pxa_http_ring_advance(&s->stream.receive, (size_t)n, 1);
        if (s->notify)
          s->notify(s->notify_context);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
done:
  if (transport) {
    esp_transport_close(transport);
    esp_transport_destroy(transport);
  }
  pxa_socket_stream_finish(&s->stream, error);
  if (s->notify)
    s->notify(s->notify_context);
  __atomic_sub_fetch(&esp_socket_workers, 1, __ATOMIC_ACQ_REL);
  esp_socket_release(s);
  vTaskDelete(NULL);
}
static pxa_status_t esp_socket_open(void *context,
                                    const pxa_socket_request_t *request,
                                    void **output) {
  (void)context;
  *output = NULL;
  if (__atomic_add_fetch(&esp_socket_workers, 1, __ATOMIC_ACQ_REL) >
      PXA_ESP_NET_MAX_PENDING) {
    __atomic_sub_fetch(&esp_socket_workers, 1, __ATOMIC_ACQ_REL);
    return PXA_STATUS_RESOURCE_LIMIT;
  }
  esp_socket_t *s = calloc(1, sizeof(*s) + request->buffer_bytes);
  if (!s)
    goto fail;
  if (request->host.size >= sizeof(s->host) ||
      !pxa_socket_stream_init(&s->stream, s->storage, request->buffer_bytes))
    goto fail;
  s->references = 2;
  s->timeout_ms = request->connect_timeout_ms;
  s->port = request->port;
  s->tls = request->tls;
  s->notify = g_lazy_notify;
  s->notify_context = g_lazy_notify_context;
  memcpy(s->host, request->host.data, request->host.size);
  if (xTaskCreate(esp_socket_worker, "pxa_socket", 6144, s,
                  PXA_ESP_NET_TASK_PRIORITY, NULL) != pdPASS)
    goto fail;
  *output = s;
  return PXA_STATUS_OK;
fail:
  free(s);
  __atomic_sub_fetch(&esp_socket_workers, 1, __ATOMIC_ACQ_REL);
  return PXA_STATUS_RESOURCE_LIMIT;
}
static int32_t esp_socket_io(void *context, void *stream, uint32_t operation,
                             uint8_t *data, size_t size) {
  (void)context;
  esp_socket_t *s = stream;
  return pxa_socket_stream_io(&s->stream, operation, data, size);
}
static void esp_socket_close(void *context, void *stream) {
  (void)context;
  esp_socket_t *s = stream;
  __atomic_fetch_or(&s->stream.state, PXA_SOCKET_CLOSED, __ATOMIC_RELEASE);
  esp_socket_release(s);
}
