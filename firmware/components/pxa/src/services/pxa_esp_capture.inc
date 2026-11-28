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
  StaticSemaphore_t mutex_storage;
  SemaphoreHandle_t mutex;
} capture;

/* Source callbacks are bounded Host operations. Serialize them separately
 * from playback so suspend/reset cannot lose a recording still opening. */
static SemaphoreHandle_t capture_mutex(void) {
  portENTER_CRITICAL(&g_audio.backend_lock);
  if (!capture.mutex)
    capture.mutex = xSemaphoreCreateMutexStatic(&capture.mutex_storage);
  portEXIT_CRITICAL(&g_audio.backend_lock);
  return capture.mutex;
}
static void capture_lock(void) { xSemaphoreTake(capture_mutex(), portMAX_DELAY); }
static void capture_unlock(void) { xSemaphoreGive(capture.mutex); }
static void capture_close_active_locked(void) {
  if (capture.active) {
    capture.active = 0;
    capture.close(capture.context);
  }
}
void pxa_esp_audio_set_capture_source(
    void *context, uint16_t frame_ms, pxa_status_t (*open)(void *),
    int32_t (*read)(void *, uint8_t *, size_t), void (*close)(void *)) {
  capture_lock();
  capture_close_active_locked();
  capture.context = context;
  capture.frame_ms = frame_ms;
  capture.open = open;
  capture.read = read;
  capture.close = close;
  capture_unlock();
}
static uint16_t capture_frame_ms(void *context) {
  (void)context;
  capture_lock();
  uint16_t frame_ms = capture.frame_ms;
  capture_unlock();
  return frame_ms;
}
static void capture_close(void *context, uint64_t id) {
  (void)context;
  capture_lock();
  if (id && capture.active == id) capture_close_active_locked();
  capture_unlock();
}
static void capture_reset_sessions(void) {
  capture_lock();
  capture_close_active_locked();
  capture_unlock();
}
static void capture_set_suspended(bool suspended) {
  capture_lock();
  capture.suspended = suspended;
  if (suspended) capture_close_active_locked();
  capture_unlock();
}
static pxa_status_t capture_open(void *context, uint64_t *id) {
  (void)context;
  *id = 0;
  capture_lock();
  pxa_status_t status;
  if (!capture.open || !capture.read || !capture.close)
    status = PXA_STATUS_UNSUPPORTED;
  else if (capture.suspended)
    status = PXA_STATUS_UNAVAILABLE;
  else if (capture.active)
    status = PXA_STATUS_BUSY;
  else {
    status = capture.open(capture.context);
    if (status == PXA_STATUS_OK) {
      if (!++capture.sequence) ++capture.sequence;
      *id = capture.active = capture.sequence;
    }
  }
  capture_unlock();
  return status;
}
static int32_t capture_io(void *context, uint64_t id, uint32_t op,
                          uint8_t *data, size_t size) {
  (void)context;
  if (xSemaphoreTake(capture_mutex(), 0) != pdTRUE) return PXA_STATUS_WOULD_BLOCK;
  int32_t result;
  if (capture.active != id || !id || capture.suspended)
    result = PXA_STATUS_UNAVAILABLE;
  else if (op != PXA_IO_READ)
    result = PXA_STATUS_UNSUPPORTED;
  else if (!data || size < PXA_AUDIO_CAPTURE_MAX_PACKET)
    result = PXA_STATUS_LIMIT_EXCEEDED;
  else
    result = capture.read(capture.context, data, size);
  capture_unlock();
  return result;
}
