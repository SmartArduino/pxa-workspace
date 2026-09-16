#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pxa/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional product hook for transports that are not exposed through the ESP
 * socket stack, such as a cellular modem running its AT HTTP client. The PXA
 * host calls it only from its dedicated network worker. Returning NOT_USED
 * lets the host continue with esp_http_client. */
enum {
    PXA_PLATFORM_NET_FETCH_NOT_USED = 0,
    PXA_PLATFORM_NET_FETCH_OK = 1,
    PXA_PLATFORM_NET_FETCH_FAILED = -1,
    PXA_PLATFORM_NET_FETCH_RESPONSE_TOO_LARGE = -2,
};

int pxa_platform_net_fetch_get(const char *url, uint32_t timeout_ms,
                               uint8_t *body, size_t body_capacity,
                               size_t *body_size, uint16_t *status_code,
                               char *content_type,
                               size_t content_type_capacity);

/* Extended v1.1 hook. All request views are borrowed for this synchronous
 * worker-thread call. Response header values are copied into header_storage;
 * their names may refer to the request's wanted_response_headers. */
int pxa_platform_net_http_request(
    const pxa_net_request_t *request, uint8_t *body, size_t body_capacity,
    size_t *body_size, uint16_t *status_code, char *content_type,
    size_t content_type_capacity, pxa_net_header_t *response_headers,
    size_t response_header_capacity, size_t *response_header_count,
    uint8_t *header_storage, size_t header_storage_capacity,
    size_t *header_storage_size);

#ifdef __cplusplus
}
#endif
