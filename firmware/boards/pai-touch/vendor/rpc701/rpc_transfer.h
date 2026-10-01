#ifndef RPC_TRANSFER_H
#define RPC_TRANSFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Application layer provides send interface
typedef int (*transfer_write_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

#ifdef __cplusplus
}
#endif
#endif
