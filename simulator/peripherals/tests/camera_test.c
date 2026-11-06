/* SPDX-License-Identifier: Apache-2.0 */
#define PXSYS_DESKTOP_HOST_EXTENSIONS 1
#include "../desktop_camera.c"
#include <assert.h>

static void wait_worker(void) {
  struct timespec delay = {0, 10000000};
  nanosleep(&delay, NULL);
}
static void *raw_allocate(void *ctx, size_t bytes) {
  (void)ctx;
  return malloc(bytes);
}
static void raw_release(void *ctx, void *bytes) {
  (void)ctx;
  free(bytes);
}
static void probe_camera(void) {
  camera_t c = {.fd = -1};
  pxa_memory_budget_t budget;
  pxa_memory_owner_t owner;
  const pxa_memory_budget_config_t config = {.limit = {0, 1024 * 1024}};
  assert(pxa_memory_budget_init(&budget, &config) == PXA_STATUS_OK);
  assert(pxa_memory_owner_open(&budget, config.limit, &owner) == PXA_STATUS_OK);
  const pxa_memory_allocator_t allocator = {.budget = &budget,
                                            .owner = owner,
                                            .memory_class = PXA_MEMORY_EXTERNAL,
                                            .kind = PXA_MEMORY_FRAME,
                                            .allocate = raw_allocate,
                                            .release = raw_release};
  c.config.frame_allocator = &allocator;
  assert(!pthread_mutex_init(&c.mutex, NULL));
  assert(!pthread_cond_init(&c.condition, NULL));
  discover(&c);
  assert(c.count);
  assert(!pthread_create(&c.worker, NULL, worker, &c));
  for (unsigned cycle = 0; cycle < 2; ++cycle) {
    void *device = NULL;
    assert(open_camera(&c, 1, 3, PXA_PERIPHERALS_MODE_STREAM, 0, 0, &device) ==
           PXA_STATUS_OK);
    for (unsigned n = 0; n < 500 && ready(&c, device) == PXA_STATUS_WOULD_BLOCK;
         n++)
      wait_worker();
    assert(ready(&c, device) == PXA_STATUS_OK);
    uint8_t bytes[PXA_PERIPHERALS_MAX_TRANSFER];
    size_t received = 0, total = 0;
    for (unsigned n = 0; n < 1000 && (!total || received < total); n++) {
      int size = read_camera(&c, device, PXA_IO_READ, bytes, sizeof(bytes));
      if (size == PXA_STATUS_WOULD_BLOCK) {
        wait_worker();
        continue;
      }
      assert(size > 0);
      if (!received) {
        pxa_vision_frame_info_t info;
        assert(pxa_vision_decode(bytes, &info));
        total = PXA_VISION_HEADER_BYTES + info.size;
      }
      received += (size_t)size;
    }
    assert(total && received == total);
    close_camera(&c, device);
    int closed = 0;
    for (unsigned n = 0; n < 500; n++) {
      pthread_mutex_lock(&c.mutex);
      closed = !c.closing && !c.busy;
      pthread_mutex_unlock(&c.mutex);
      if (closed)
        break;
      wait_worker();
    }
    assert(closed && c.fd == -1 && !c.streaming && !c.frame && !c.map_count);
    pxa_memory_stats_t stats;
    assert(pxa_memory_budget_stats(&budget, 0, &stats) == PXA_STATUS_OK);
    assert(stats.charged[PXA_MEMORY_EXTERNAL] == 0);
  }
  pthread_mutex_lock(&c.mutex);
  c.stopping = 1;
  pthread_cond_signal(&c.condition);
  pthread_mutex_unlock(&c.mutex);
  pthread_join(c.worker, NULL);
  pthread_cond_destroy(&c.condition);
  pthread_mutex_destroy(&c.mutex);
  assert(pxa_memory_owner_close(&budget, owner) == PXA_STATUS_OK);
  puts("Real system camera: two open/capture/close cycles, DMA mappings and FD "
       "released while worker remains alive; budget returns to zero");
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--camera")) {
    probe_camera();
    return 0;
  }
  camera_t c = {0};
  assert(!pthread_mutex_init(&c.mutex, NULL));
  assert(!pthread_cond_init(&c.condition, NULL));
#if defined(__linux__)
  add_profile(&c, 320, 240, V4L2_PIX_FMT_MJPEG);
  add_profile(&c, 320, 240, V4L2_PIX_FMT_YUYV);
  add_profile(&c, UINT32_MAX, 240, V4L2_PIX_FMT_MJPEG);
  add_profile(&c, 319, 240, V4L2_PIX_FMT_YUYV);
  assert(c.count == 1 && c.profiles[0].format == V4L2_PIX_FMT_MJPEG);
  assert(c.profiles[0].public.id == ((320u << 16) | 240));
#endif
  void *device = (void *)1;
  assert(open_camera(&c, 1, 3, PXA_PERIPHERALS_MODE_STREAM, 999, 0, &device) ==
             PXA_STATUS_UNSUPPORTED &&
         !device);
  assert(!c.live && !c.opening);
  uint8_t bytes[17], frame[64];
  for (unsigned i = 0; i < sizeof(frame); i++)
    frame[i] = (uint8_t)i;
  c.live = 1;
  c.status = PXA_STATUS_OK;
  c.frame = frame;
  c.used = sizeof(frame);
  size_t offset = 0;
  while (offset < sizeof(frame)) {
    int n = read_camera(&c, &c, PXA_IO_READ, bytes, sizeof(bytes));
    assert(n > 0 && memcmp(bytes, frame + offset, (size_t)n) == 0);
    offset += (size_t)n;
  }
  assert(!c.used && !c.offset);
  c.busy = 1;
  for (unsigned i = 0; i < 100; i++)
    assert(read_camera(&c, &c, PXA_IO_READ, bytes, sizeof(bytes)) ==
           PXA_STATUS_WOULD_BLOCK);
  assert(
      !c.requested); /* No second job may overwrite a frame being delivered. */
  c.busy = 0;
  assert(read_camera(&c, &c, PXA_IO_READ, bytes, sizeof(bytes)) ==
         PXA_STATUS_WOULD_BLOCK);
  assert(c.requested);
  close_camera(&c, &c);
  assert(!c.live && c.closing && !c.requested);
  assert(read_camera(&c, &c, PXA_IO_READ, bytes, sizeof(bytes)) ==
         PXA_STATUS_CANCELLED);
#if defined(__linux__)
  uint8_t yuyv[] = {81, 90, 81, 240, 81, 90, 81, 240};
  uint8_t *encoded = malloc(PXA_VISION_HEADER_BYTES + PXA_VISION_MAX_JPEG);
  assert(encoded);
  c.frame = encoded;
  c.selected = 0;
  c.profiles[0].public.width = 2;
  c.profiles[0].public.height = 2;
  c.stride = 4;
  c.quantization = V4L2_QUANTIZATION_LIM_RANGE;
  c.ycbcr = V4L2_YCBCR_ENC_601;
  size_t size = 0;
  assert(encode_yuyv(&c, yuyv, sizeof(yuyv) - 1, &size) ==
         PXA_STATUS_PROTOCOL_ERROR);
  assert(encode_yuyv(&c, yuyv, sizeof(yuyv), &size) == PXA_STATUS_OK);
  struct jpeg_decompress_struct j = {0};
  struct jpeg_error_mgr error;
  j.err = jpeg_std_error(&error);
  jpeg_create_decompress(&j);
  jpeg_mem_src(&j, encoded + PXA_VISION_HEADER_BYTES, size);
  assert(jpeg_read_header(&j, TRUE) == JPEG_HEADER_OK);
  jpeg_start_decompress(&j);
  assert(j.output_width == 2 && j.output_height == 2 &&
         j.output_components == 3);
  uint8_t rgb[6];
  JSAMPROW row[] = {rgb};
  while (j.output_scanline < j.output_height) {
    jpeg_read_scanlines(&j, row, 1);
    assert(rgb[0] > 230 && rgb[1] < 20 && rgb[2] < 20);
  }
  jpeg_finish_decompress(&j);
  jpeg_destroy_decompress(&j);
  free(encoded);
#endif
  pthread_cond_destroy(&c.condition);
  pthread_mutex_destroy(&c.mutex);
  puts("Desktop camera: capability bounds, stable IDs, immutable partial "
       "delivery, in-flight deduplication, cancellation and YUYV color "
       "conversion passed");
  return 0;
}
