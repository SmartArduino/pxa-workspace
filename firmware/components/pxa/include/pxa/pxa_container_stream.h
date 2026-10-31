#ifndef PXA_HOST_CONTAINER_STREAM_H
#define PXA_HOST_CONTAINER_STREAM_H
#include <stddef.h>
#include <stdint.h>
#include "pxa/status.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Private transfer directory, exclusively owned by the transport. Close
 * discards incomplete output. Finished output is passed to normal installation. */
typedef struct pxa_host_container_stream pxa_host_container_stream_t;
pxa_status_t pxa_host_container_stream_begin(const char *directory,uint64_t bytes,
    const uint8_t *sha256,pxa_host_container_stream_t **);
/* PXADB owns one private cache directory on this exact mount instance.
 * Close also discards finished, uncommitted output and releases the volume. */
pxa_status_t pxa_host_container_stream_begin_volume(uint64_t epoch,const char *directory,
    uint64_t bytes,const uint8_t sha256[32],pxa_host_container_stream_t **);
/* Checks the canonical volume-local cache namespace; does not authenticate it. */
int pxa_host_container_stream_volume_path(uint64_t epoch,const char *directory);
pxa_status_t pxa_host_container_stream_feed(pxa_host_container_stream_t *,const uint8_t *,size_t);
pxa_status_t pxa_host_container_stream_finish(pxa_host_container_stream_t *);
/* Fetch signed container bytes from an explicitly supplied HTTP URL. No
 * redirects; success includes full container and file verification. */
pxa_status_t pxa_host_container_stream_fetch(pxa_host_container_stream_t *,const char *url);
void pxa_host_container_stream_close(pxa_host_container_stream_t *);
#ifdef __cplusplus
}
#endif
#endif
