#ifndef RPC_701_H
#define RPC_701_H

#include <stddef.h>
#include <stdint.h>
#include "rpc_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUCCESS 0
#define FAILURE -1

typedef struct {
    transfer_write_cb_t write_cb;
} rpc_701_config_t;

int rpc_701_init(const rpc_701_config_t *config);
void rpc_701_deinit(void);

/* Audio OPUS Frame Buffer API */
int audio_opus_frame_write(const uint8_t *data, size_t len);
int audio_opus_frame_read(uint8_t *data, size_t max_len);
size_t audio_opus_frame_available(void);
void audio_opus_frame_flush(void);

/* Feed bytes received from the underlying transport (e.g. UART) into rpc_701. */
void rpc_701_feed_rx(uint8_t *data, size_t len);
/* Reset rpc_701 receive parser (e.g. after a UART error). */
void rpc_701_reset_rx(void);

#ifdef __cplusplus
}
#endif

#endif /* RPC_701_H */
