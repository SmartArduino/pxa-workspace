/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "host_extensions.h"
#include <errno.h>
#include <pthread.h>
#include <pxa_ext/peripherals.h>
#include <pxa_ext/vision_frame.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
/* Linux V4L2 UAPI declarations: select their BSD-3-Clause licensing branch.
 * No libv4l/FFmpeg/kernel implementation is linked or copied. */
#include <fcntl.h>
#include <jerror.h>
#include <jpeglib.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum { MAX_WIDTH = 1280, MAX_HEIGHT = 720, MAX_MAPS = 4 };
typedef struct {
  pxa_camera_profile_t public;
  uint32_t format;
} profile_t;
typedef struct {
  pxsys_desktop_host_extensions_config_t config;
  pxa_peripherals_service_t *service;
  void *workspace;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  pthread_t worker;
  int stopping, live, closing, opening, requested, busy, thread_ready;
  pxa_status_t status;
  char path[256];
  profile_t profiles[PXA_CAMERA_MAX_PROFILES];
  size_t count, selected, used, offset;
  uint32_t sequence;
  uint8_t *frame;
  int fd, streaming;
  struct {
    void *data;
    size_t size;
  } maps[MAX_MAPS];
  unsigned map_count;
  uint32_t stride, quantization, ycbcr;
} camera_t;
static uint64_t now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static inline int cancelled(camera_t *c) {
  pthread_mutex_lock(&c->mutex);
  int stop = c->stopping || c->closing;
  pthread_mutex_unlock(&c->mutex);
  return stop;
}
#if defined(__linux__)
static int control(int fd, unsigned long op, void *value) {
  int result;
  do {
    result = ioctl(fd, op, value);
  } while (result < 0 && errno == EINTR);
  return result;
}
static pxa_status_t io_error(void) {
  return errno == EACCES || errno == EPERM ? PXA_STATUS_DENIED
         : errno == EBUSY                  ? PXA_STATUS_BUSY
                                           : PXA_STATUS_IO_ERROR;
}
static int valid_device(int fd) {
  struct stat s;
  struct v4l2_capability caps = {0};
  if (fstat(fd, &s) || !S_ISCHR(s.st_mode) ||
      control(fd, VIDIOC_QUERYCAP, &caps))
    return 0;
  uint32_t flags = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps
                                                            : caps.capabilities;
  return (flags & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)) ==
         (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING);
}
static void add_profile(camera_t *c, uint32_t w, uint32_t h, uint32_t format) {
  if (!w || !h || w > MAX_WIDTH || h > MAX_HEIGHT || (w & 1))
    return;
  for (size_t i = 0; i < c->count; i++)
    if (c->profiles[i].public.width == w && c->profiles[i].public.height == h)
      return;
  if (c->count == PXA_CAMERA_MAX_PROFILES)
    return;
  c->profiles[c->count++] = (profile_t){{(w << 16) | h, PXA_VISION_JPEG, w, h,
                                         format == V4L2_PIX_FMT_MJPEG ? 0 : 80,
                                         PXA_VISION_MAX_JPEG, 0, 0},
                                        format};
}
static void discover(camera_t *c) {
  const char *choice = getenv("PXA_SIM_CAMERA_DEVICE");
  if (choice && (!strcmp(choice, "off") || !strcmp(choice, "none")))
    return;
  for (unsigned device = 0; device < 32; device++) {
    char path[256];
    if (choice) {
      if (strlen(choice) >= sizeof(path))
        return;
      strcpy(path, choice);
    } else
      snprintf(path, sizeof(path), "/dev/video%u", device);
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0 && valid_device(fd)) {
      /* Prefer native JPEG over YUYV at the same size. Discovery never
       * negotiates a format, allocates DMA or starts streaming. */
      for (unsigned pass = 0; pass < 2; pass++) {
        const char *preferred = getenv("PXA_SIM_CAMERA_FORMAT");
        if (preferred && strcmp(preferred, pass ? "yuyv" : "mjpeg"))
          continue;
        uint32_t format = pass ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
        for (unsigned i = 0; i < 64; i++) {
          struct v4l2_frmsizeenum size = {.index = i, .pixel_format = format};
          if (control(fd, VIDIOC_ENUM_FRAMESIZES, &size))
            break;
          if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            add_profile(c, size.discrete.width, size.discrete.height, format);
          else {
            const unsigned wanted[][2] = {{320, 240}, {640, 480}, {1280, 720}};
            for (unsigned j = 0; j < 3; j++) {
              uint32_t w = wanted[j][0], h = wanted[j][1];
              if (w >= size.stepwise.min_width &&
                  w <= size.stepwise.max_width &&
                  h >= size.stepwise.min_height &&
                  h <= size.stepwise.max_height &&
                  (!size.stepwise.step_width ||
                   (w - size.stepwise.min_width) % size.stepwise.step_width ==
                       0) &&
                  (!size.stepwise.step_height ||
                   (h - size.stepwise.min_height) % size.stepwise.step_height ==
                       0))
                add_profile(c, w, h, format);
            }
            break;
          }
        }
      }
      if (c->count)
        strcpy(c->path, path);
    }
    if (fd >= 0)
      close(fd);
    if (c->count || choice)
      break;
  }
  if (!c->count)
    return;
  size_t selected = 0;
  for (size_t i = 0; i < c->count; i++)
    if (c->profiles[i].public.width == 320 &&
        c->profiles[i].public.height == 240)
      selected = i;
  c->profiles[selected].public.flags = PXA_CAMERA_PROFILE_DEFAULT;
  fprintf(stderr, "PXA CAMERA device=%s profiles=%zu default=%ux%u\n", c->path,
          c->count, c->profiles[selected].public.width,
          c->profiles[selected].public.height);
}
static void native_close(camera_t *c) {
  if (c->fd >= 0 && c->streaming) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)control(c->fd, VIDIOC_STREAMOFF, &type);
  }
  c->streaming = 0;
  for (unsigned i = 0; i < c->map_count; i++)
    if (c->maps[i].data)
      munmap(c->maps[i].data, c->maps[i].size);
  memset(c->maps, 0, sizeof(c->maps));
  c->map_count = 0;
  if (c->fd >= 0)
    close(c->fd);
  c->fd = -1;
  pxa_memory_release(c->frame);
  c->frame = NULL;
}
static pxa_status_t native_open(camera_t *c) {
  const profile_t p = c->profiles[c->selected];
  c->fd = open(c->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (c->fd < 0)
    return io_error();
  if (!valid_device(c->fd))
    return PXA_STATUS_UNAVAILABLE;
  struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
  format.fmt.pix.width = p.public.width;
  format.fmt.pix.height = p.public.height;
  format.fmt.pix.pixelformat = p.format;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  if (control(c->fd, VIDIOC_S_FMT, &format))
    return io_error();
  if (format.fmt.pix.width != p.public.width ||
      format.fmt.pix.height != p.public.height ||
      format.fmt.pix.pixelformat != p.format)
    return PXA_STATUS_UNSUPPORTED;
  c->stride = format.fmt.pix.bytesperline;
  c->quantization = format.fmt.pix.quantization;
  c->ycbcr = format.fmt.pix.ycbcr_enc;
  if (c->ycbcr == V4L2_YCBCR_ENC_DEFAULT)
    c->ycbcr = V4L2_MAP_YCBCR_ENC_DEFAULT(format.fmt.pix.colorspace);
  if (c->quantization == V4L2_QUANTIZATION_DEFAULT)
    c->quantization =
        V4L2_MAP_QUANTIZATION_DEFAULT(0, format.fmt.pix.colorspace, c->ycbcr);
  if (p.format == V4L2_PIX_FMT_YUYV &&
      (c->stride < p.public.width * 2u || c->stride > MAX_WIDTH * 8u))
    return PXA_STATUS_PROTOCOL_ERROR;
  c->frame = pxa_memory_allocate(c->config.frame_allocator,
                                 PXA_VISION_HEADER_BYTES + PXA_VISION_MAX_JPEG);
  if (!c->frame)
    return PXA_STATUS_RESOURCE_LIMIT;
  struct v4l2_requestbuffers request = {.count = 2,
                                        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                        .memory = V4L2_MEMORY_MMAP};
  if (control(c->fd, VIDIOC_REQBUFS, &request))
    return io_error();
  if (request.count < 2 || request.count > MAX_MAPS)
    return PXA_STATUS_RESOURCE_LIMIT;
  for (unsigned i = 0; i < request.count; i++) {
    struct v4l2_buffer b = {
        .index = i, .type = request.type, .memory = request.memory};
    if (control(c->fd, VIDIOC_QUERYBUF, &b))
      return io_error();
    if (!b.length || b.length > 4u * 1024u * 1024u)
      return PXA_STATUS_RESOURCE_LIMIT;
    void *mapped = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        c->fd, b.m.offset);
    if (mapped == MAP_FAILED)
      return io_error();
    c->maps[c->map_count].data = mapped;
    c->maps[c->map_count++].size = b.length;
    if (control(c->fd, VIDIOC_QBUF, &b))
      return io_error();
  }
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (control(c->fd, VIDIOC_STREAMON, &type))
    return io_error();
  c->streaming = 1;
  fprintf(stderr, "PXA CAMERA streaming=%s size=%ux%u format=%s\n", c->path,
          p.public.width, p.public.height,
          p.format == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV");
  return PXA_STATUS_OK;
}
typedef struct {
  struct jpeg_error_mgr base;
  jmp_buf jump;
} jpeg_error_t;
static void jpeg_error(j_common_ptr j) {
  longjmp(((jpeg_error_t *)j->err)->jump, 1);
}
static void destination_init(j_compress_ptr j) { (void)j; }
static boolean destination_full(j_compress_ptr j) {
  ERREXIT(j, JERR_BUFFER_SIZE);
  return FALSE;
}
static void destination_finish(j_compress_ptr j) { (void)j; }
static uint8_t channel(int n) { return n < 0 ? 0 : n > 255 ? 255 : (uint8_t)n; }
static pxa_status_t encode_yuyv(camera_t *c, const uint8_t *bytes, size_t size,
                                size_t *out) {
  const profile_t p = c->profiles[c->selected];
  if (size < (size_t)c->stride * p.public.height)
    return PXA_STATUS_PROTOCOL_ERROR;
  struct jpeg_compress_struct j = {0};
  jpeg_error_t error;
  j.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpeg_error;
  if (setjmp(error.jump)) {
    jpeg_destroy_compress(&j);
    return PXA_STATUS_LIMIT_EXCEEDED;
  }
  jpeg_create_compress(&j);
  struct jpeg_destination_mgr dest = {.next_output_byte =
                                          c->frame + PXA_VISION_HEADER_BYTES,
                                      .free_in_buffer = PXA_VISION_MAX_JPEG,
                                      .init_destination = destination_init,
                                      .empty_output_buffer = destination_full,
                                      .term_destination = destination_finish};
  j.dest = &dest;
  j.image_width = p.public.width;
  j.image_height = p.public.height;
  j.input_components = 3;
  j.in_color_space = JCS_RGB;
  jpeg_set_defaults(&j);
  jpeg_set_quality(&j, 80, TRUE);
  jpeg_start_compress(&j, TRUE);
  uint8_t row[MAX_WIDTH * 3];
  while (j.next_scanline < j.image_height) {
    const uint8_t *in = bytes + (size_t)j.next_scanline * c->stride;
    for (unsigned x = 0; x < p.public.width; x++) {
      int y = in[(x & ~1u) * 2 + (x & 1u) * 2], u = in[(x & ~1u) * 2 + 1] - 128,
          v = in[(x & ~1u) * 2 + 3] - 128;
      int full = c->quantization == V4L2_QUANTIZATION_FULL_RANGE;
      int base = full ? y * 256 : (y - 16) * 298;
      int hd = c->ycbcr == V4L2_YCBCR_ENC_709;
      row[x * 3] = channel(
          (base + (hd ? (full ? 403 : 459) : (full ? 359 : 409)) * v + 128) >>
          8);
      row[x * 3 + 1] =
          channel((base - (hd ? (full ? 48 : 55) : (full ? 88 : 100)) * u -
                   (hd ? (full ? 120 : 136) : (full ? 183 : 208)) * v + 128) >>
                  8);
      row[x * 3 + 2] = channel(
          (base + (hd ? (full ? 475 : 541) : (full ? 454 : 516)) * u + 128) >>
          8);
    }
    JSAMPROW rows[] = {row};
    jpeg_write_scanlines(&j, rows, 1);
  }
  jpeg_finish_compress(&j);
  *out = PXA_VISION_MAX_JPEG - dest.free_in_buffer;
  jpeg_destroy_compress(&j);
  return PXA_STATUS_OK;
}
static pxa_status_t native_capture(camera_t *c, size_t *size) {
  uint64_t deadline = now_us() + 3000000;
  while (now_us() < deadline && !cancelled(c)) {
    struct pollfd wait = {.fd = c->fd, .events = POLLIN};
    int result = poll(&wait, 1, 100);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return io_error();
    }
    if (!result)
      continue;
    if (wait.revents & (POLLERR | POLLHUP | POLLNVAL))
      return PXA_STATUS_UNAVAILABLE;
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            .memory = V4L2_MEMORY_MMAP};
    if (control(c->fd, VIDIOC_DQBUF, &b)) {
      if (errno == EAGAIN)
        continue;
      return io_error();
    }
    pxa_status_t status = PXA_STATUS_PROTOCOL_ERROR;
    if (b.index < c->map_count && b.bytesused <= c->maps[b.index].size &&
        !(b.flags & V4L2_BUF_FLAG_ERROR)) {
      const uint8_t *bytes = c->maps[b.index].data;
      size_t n = b.bytesused;
      if (c->profiles[c->selected].format == V4L2_PIX_FMT_MJPEG) {
        while (n > 2 && !(bytes[n - 2] == 0xff && bytes[n - 1] == 0xd9))
          --n;
        if (n >= 4 && n <= PXA_VISION_MAX_JPEG && bytes[0] == 0xff &&
            bytes[1] == 0xd8) {
          memcpy(c->frame + PXA_VISION_HEADER_BYTES, bytes, n);
          *size = n;
          status = PXA_STATUS_OK;
        }
      } else
        status = encode_yuyv(c, bytes, n, size);
    }
    if (control(c->fd, VIDIOC_QBUF, &b))
      return io_error();
    if (status == PXA_STATUS_OK)
      return status;
  }
  return cancelled(c) ? PXA_STATUS_CANCELLED : PXA_STATUS_TIMED_OUT;
}
#else
static void discover(camera_t *c) { (void)c; }
static void native_close(camera_t *c) {
  pxa_memory_release(c->frame);
  c->frame = NULL;
}
static pxa_status_t native_open(camera_t *c) {
  (void)c;
  return PXA_STATUS_UNSUPPORTED;
}
static pxa_status_t native_capture(camera_t *c, size_t *n) {
  (void)c;
  (void)n;
  return PXA_STATUS_UNSUPPORTED;
}
#endif
static void *worker(void *context) {
  camera_t *c = context;
  pthread_mutex_lock(&c->mutex);
  while (!c->stopping) {
    while (!c->stopping && !c->closing && !c->opening && !c->requested)
      pthread_cond_wait(&c->condition, &c->mutex);
    if (c->stopping)
      break;
    int closing = c->closing, opening = c->opening;
    c->opening = c->requested = 0;
    c->busy = 1;
    pthread_mutex_unlock(&c->mutex);
    size_t size = 0;
    pxa_status_t status = closing   ? PXA_STATUS_CANCELLED
                          : opening ? native_open(c)
                                    : native_capture(c, &size);
    if (closing || status != PXA_STATUS_OK)
      native_close(c);
    pthread_mutex_lock(&c->mutex);
    c->busy = 0;
    if (closing)
      c->closing = 0;
    if (c->closing) {
      pthread_cond_signal(&c->condition);
      continue;
    }
    c->status = status;
    if (size && status == PXA_STATUS_OK) {
      const pxa_vision_frame_info_t info = {PXA_VISION_JPEG, (uint32_t)size,
                                            ++c->sequence, now_us()};
      pxa_vision_encode(c->frame, &info);
      c->offset = 0;
      c->used = PXA_VISION_HEADER_BYTES + size;
    }
  }
  pthread_mutex_unlock(&c->mutex);
  native_close(c);
  return NULL;
}
static pxa_status_t list(void *ctx, pxa_peripheral_port_t *p, size_t capacity,
                         size_t *count) {
  camera_t *c = ctx;
  *count = c->count ? 1 : 0;
  if (capacity < *count)
    return PXA_STATUS_LIMIT_EXCEEDED;
  if (*count) {
    *p = (pxa_peripheral_port_t){
        .id = 3, .kind = PXA_PERIPHERALS_VISION, .flags = PXA_PERIPHERALS_READ};
    strcpy(p->name, "camera.jpeg");
  }
  return PXA_STATUS_OK;
}
static pxa_status_t module(void *ctx, uint16_t port,
                           pxa_peripheral_module_t *m) {
  (void)ctx;
  (void)port;
  (void)m;
  return PXA_STATUS_NOT_FOUND;
}
static pxa_status_t profiles(void *ctx, uint16_t port, pxa_camera_profile_t *p,
                             size_t capacity, size_t *count) {
  camera_t *c = ctx;
  *count = c->count;
  if (port != 3)
    return PXA_STATUS_NOT_FOUND;
  if (capacity < c->count)
    return PXA_STATUS_LIMIT_EXCEEDED;
  for (size_t i = 0; i < c->count; i++)
    p[i] = c->profiles[i].public;
  return PXA_STATUS_OK;
}
static pxa_status_t open_camera(void *ctx, uint64_t owner, uint16_t port,
                                uint16_t mode, uint32_t profile,
                                uint16_t address, void **device) {
  camera_t *c = ctx;
  *device = NULL;
  if (!owner || port != 3 || mode != PXA_PERIPHERALS_MODE_STREAM || address)
    return PXA_STATUS_INVALID_ARGUMENT;
  size_t selected = c->count;
  for (size_t i = 0; i < c->count; i++)
    if (profile ? c->profiles[i].public.id == profile
                : !!(c->profiles[i].public.flags & PXA_CAMERA_PROFILE_DEFAULT))
      selected = i;
  if (selected == c->count)
    return PXA_STATUS_UNSUPPORTED;
  pthread_mutex_lock(&c->mutex);
  if (c->live || c->closing || c->busy) {
    pthread_mutex_unlock(&c->mutex);
    return PXA_STATUS_BUSY;
  }
  c->selected = selected;
  c->live = c->opening = 1;
  c->status = PXA_STATUS_WOULD_BLOCK;
  c->used = c->offset = 0;
  *device = c;
  pthread_cond_signal(&c->condition);
  pthread_mutex_unlock(&c->mutex);
  return PXA_STATUS_OK;
}
static pxa_status_t ready(void *ctx, void *device) {
  camera_t *c = ctx;
  if (device != c)
    return PXA_STATUS_INVALID_ARGUMENT;
  pthread_mutex_lock(&c->mutex);
  pxa_status_t status = c->live ? c->status : PXA_STATUS_CANCELLED;
  pthread_mutex_unlock(&c->mutex);
  return status;
}
static int32_t read_camera(void *ctx, void *device, uint32_t op, uint8_t *bytes,
                           size_t size) {
  camera_t *c = ctx;
  if (device != c || !bytes || !size)
    return PXA_STATUS_INVALID_ARGUMENT;
  if (op != PXA_IO_READ)
    return PXA_STATUS_UNSUPPORTED;
  pthread_mutex_lock(&c->mutex);
  int32_t result = c->live ? c->status : PXA_STATUS_CANCELLED;
  if (result == PXA_STATUS_OK) {
    if (!c->used) {
      if (!c->busy && !c->requested) {
        c->requested = 1;
        pthread_cond_signal(&c->condition);
      }
      result = PXA_STATUS_WOULD_BLOCK;
    } else {
      size_t n = c->used - c->offset;
      if (n > size)
        n = size;
      memcpy(bytes, c->frame + c->offset, n);
      c->offset += n;
      if (c->offset == c->used)
        c->offset = c->used = 0;
      result = (int32_t)n;
    }
  }
  pthread_mutex_unlock(&c->mutex);
  return result;
}
static void close_camera(void *ctx, void *device) {
  camera_t *c = ctx;
  if (device != c)
    return;
  pthread_mutex_lock(&c->mutex);
  c->live = 0;
  c->closing = 1;
  c->requested = 0;
  pthread_cond_signal(&c->condition);
  pthread_mutex_unlock(&c->mutex);
}
static pxa_status_t start_io(void *ctx, void *device, uint64_t cookie,
                             const uint8_t *tx, uint16_t tx_size,
                             uint16_t rx_size) {
  (void)ctx;
  (void)device;
  (void)cookie;
  (void)tx;
  (void)tx_size;
  (void)rx_size;
  return PXA_STATUS_UNSUPPORTED;
}
static pxa_status_t poll_io(void *ctx, void *device, uint64_t cookie,
                            uint8_t *bytes, size_t *size) {
  (void)ctx;
  (void)device;
  (void)cookie;
  (void)bytes;
  (void)size;
  return PXA_STATUS_UNSUPPORTED;
}
static void cancel_io(void *ctx, void *device, uint64_t cookie) {
  (void)ctx;
  (void)device;
  (void)cookie;
}
void *pxsys_desktop_host_extensions_create(
    const pxsys_desktop_host_extensions_config_t *config) {
  if (!config || !config->runtime || !config->permissions)
    return NULL;
  camera_t *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  c->config = *config;
  c->fd = -1;
  if (pthread_mutex_init(&c->mutex, NULL)) {
    free(c);
    return NULL;
  }
  if (pthread_cond_init(&c->condition, NULL)) {
    pthread_mutex_destroy(&c->mutex);
    free(c);
    return NULL;
  }
  discover(c);
  if (pthread_create(&c->worker, NULL, worker, c))
    goto failed;
  c->thread_ready = 1;
  c->workspace = calloc(1, pxa_peripherals_service_workspace_size());
  const pxa_peripherals_backend_t backend = {.struct_size = sizeof(backend),
                                             .context = c,
                                             .list = list,
                                             .module = module,
                                             .open = open_camera,
                                             .io = read_camera,
                                             .start = start_io,
                                             .poll = poll_io,
                                             .cancel = cancel_io,
                                             .close = close_camera,
                                             .ready = ready,
                                             .camera_profiles = profiles};
  if (!c->workspace ||
      pxa_peripherals_service_init(
          c->workspace, pxa_peripherals_service_workspace_size(),
          config->runtime, config->permissions, &backend, &c->service))
    goto failed;
  pxa_peripherals_service_set_assets(c->service, config->assets);
  if (pxa_peripherals_service_register(c->service))
    goto failed;
  return c;
failed:
  pxsys_desktop_host_extensions_destroy(c);
  return NULL;
}
void pxsys_desktop_host_extensions_poll(void *ctx) {
  camera_t *c = ctx;
  if (c)
    pxa_peripherals_service_poll(c->service);
}
void pxsys_desktop_host_extensions_foreground(void *ctx,
                                              pxa_component_t component) {
  camera_t *c = ctx;
  if (c)
    pxa_peripherals_service_set_foreground(c->service, component);
}
void pxsys_desktop_host_extensions_destroy(void *ctx) {
  camera_t *c = ctx;
  if (!c)
    return;
  pthread_mutex_lock(&c->mutex);
  c->stopping = 1;
  pthread_cond_signal(&c->condition);
  pthread_mutex_unlock(&c->mutex);
  if (c->thread_ready)
    pthread_join(c->worker, NULL);
  free(c->workspace);
  pthread_cond_destroy(&c->condition);
  pthread_mutex_destroy(&c->mutex);
  free(c);
}
