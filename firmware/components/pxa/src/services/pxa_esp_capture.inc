/* SPDX-License-Identifier: MIT */
#include "pxa/pxa_audio_capture.h"
static struct {
  void *context;
  pxa_status_t (*open)(void *);
  int32_t (*read)(void *, uint8_t *, size_t);
  void (*close)(void *);
  uint64_t sequence, active;
  bool suspended;
  uint16_t frame_ms;
} capture;
void pxa_esp_audio_set_capture_source(
    void *context, uint16_t frame_ms, pxa_status_t (*open)(void *),
    int32_t (*read)(void *, uint8_t *, size_t), void (*close)(void *)) {
  capture.context = context;
  capture.frame_ms = frame_ms;
  capture.open = open;
  capture.read = read;
  capture.close = close;
}
static uint16_t capture_frame_ms(void *context) {
  (void)context;
  return capture.frame_ms;
}
static void capture_close(void *context, uint64_t id) {
  (void)context;
  if (!id || capture.active != id)
    return;
  capture.active = 0;
  capture.close(capture.context);
}
static pxa_status_t capture_open(void *context, uint64_t *id) {
  (void)context;
  *id = 0;
  if (!capture.open || !capture.read || !capture.close)
    return PXA_STATUS_UNSUPPORTED;
  if (capture.suspended)
    return PXA_STATUS_UNAVAILABLE;
  if (capture.active)
    return PXA_STATUS_BUSY;
  pxa_status_t status = capture.open(capture.context);
  if (status)
    return status;
  capture.active = ++capture.sequence;
  if (!capture.active)
    capture.active = ++capture.sequence;
  *id = capture.active;
  return PXA_STATUS_OK;
}
static int32_t capture_io(void *context, uint64_t id, uint32_t op,
                          uint8_t *data, size_t size) {
  (void)context;
  if (capture.active != id || !id || capture.suspended)
    return PXA_STATUS_UNAVAILABLE;
  if (op != PXA_IO_READ)
    return PXA_STATUS_UNSUPPORTED;
  if (!data || size < PXA_AUDIO_CAPTURE_MAX_PACKET)
    return PXA_STATUS_LIMIT_EXCEEDED;
  return capture.read(capture.context, data, size);
}
