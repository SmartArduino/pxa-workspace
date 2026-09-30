#include "pxa_esp_system_overlay.h"

#if defined(ESP_PLATFORM)

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "pxa/pxa_esp_surface.h"
#include "src/core/lv_obj_draw_private.h"

#define PXA_SYSTEM_OVERLAY_MAX_OBJECTS 8u
#define PXA_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS 8u
#define PXA_SYSTEM_OVERLAY_MAX_PIXELS (1024u * 768u)

static portMUX_TYPE g_plane_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *g_objects[PXA_SYSTEM_OVERLAY_MAX_OBJECTS];
static uint8_t g_object_count;
static lv_obj_t *g_reference_objects[PXA_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS];
static uint8_t g_reference_count;
static bool g_bound;
static uint8_t *g_plane_memory;
static pxa_esp_surface_ui_alpha_plane_t g_plane;
static uint64_t g_revision;

static bool provide_plane(void *context,
                          pxa_esp_surface_ui_alpha_plane_t *output) {
  (void)context;
  taskENTER_CRITICAL(&g_plane_lock);
  *output = g_plane;
  taskEXIT_CRITICAL(&g_plane_lock);
  return true;
}

static uint16_t pack565(uint8_t red, uint8_t green, uint8_t blue) {
  return (uint16_t)(((uint16_t)(red >> 3) << 11) |
                    ((uint16_t)(green >> 2) << 5) | (blue >> 3));
}

static uint8_t expand5(uint16_t value) {
  return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand6(uint16_t value) {
  return (uint8_t)((value << 2) | (value >> 4));
}

static void blend_pixel(uint16_t *color, uint8_t *alpha, lv_color32_t source) {
  const uint32_t source_alpha = source.alpha;
  if (source_alpha == 0)
    return;
  if (*alpha == 0 || source_alpha == 255) {
    *color = pack565(source.red, source.green, source.blue);
    *alpha = (uint8_t)source_alpha;
    return;
  }
  const uint32_t inverse = 255u - source_alpha;
  const uint32_t destination_alpha = *alpha;
  const uint32_t output_alpha =
      source_alpha + (destination_alpha * inverse + 127u) / 255u;
  const uint32_t red =
      (uint32_t)source.red * source_alpha +
      ((uint32_t)expand5(*color >> 11) * destination_alpha * inverse + 127u) /
          255u;
  const uint32_t green =
      (uint32_t)source.green * source_alpha +
      ((uint32_t)expand6((*color >> 5) & 0x3fu) * destination_alpha * inverse +
       127u) /
          255u;
  const uint32_t blue =
      (uint32_t)source.blue * source_alpha +
      ((uint32_t)expand5(*color & 0x1fu) * destination_alpha * inverse + 127u) /
          255u;
  *color =
      pack565((uint8_t)(red / output_alpha), (uint8_t)(green / output_alpha),
              (uint8_t)(blue / output_alpha));
  *alpha = (uint8_t)output_alpha;
}

static void publish_plane(uint8_t *memory, int32_t x, int32_t y, uint16_t width,
                          uint16_t height, bool fallback_visible) {
  uint8_t *old;
  pxa_esp_surface_ui_alpha_plane_t plane = {0};
  if (memory != NULL) {
    const size_t count = (size_t)width * height;
    plane.pixels = (const uint16_t *)memory;
    plane.alpha = memory + count * sizeof(uint16_t);
    plane.pixel_stride_bytes = (uint32_t)width * sizeof(uint16_t);
    plane.alpha_stride_bytes = width;
    plane.x = x;
    plane.y = y;
    plane.width = width;
    plane.height = height;
    plane.opacity = 255;
    plane.visible = 1;
  } else {
    plane.visible = fallback_visible;
  }
  plane.revision = ++g_revision;
  taskENTER_CRITICAL(&g_plane_lock);
  old = g_plane_memory;
  g_plane_memory = memory;
  g_plane = plane;
  taskEXIT_CRITICAL(&g_plane_lock);
  heap_caps_free(old);
  pxa_esp_surface_require_composition();
}

void pxa_esp_system_overlay_bind(void) {
  if (g_bound)
    return;
  g_bound = true;
  pxa_esp_surface_set_system_alpha_provider(provide_plane, NULL);
  pxa_esp_system_overlay_refresh();
}

void pxa_esp_system_overlay_refresh(void) {
  lv_obj_t *ordered[PXA_SYSTEM_OVERLAY_MAX_OBJECTS +
                    PXA_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS];
  uint8_t count = 0;
  int32_t left = INT32_MAX, top = INT32_MAX;
  int32_t right = 0, bottom = 0;
  lv_obj_t *layer;
  uint8_t *memory = NULL;
  uint16_t *colors;
  uint8_t *alpha;
  if (!g_bound)
    return;
  layer = lv_layer_top();
  lv_obj_update_layout(layer);
  const int32_t display_width =
      lv_display_get_horizontal_resolution(lv_display_get_default());
  const int32_t display_height =
      lv_display_get_vertical_resolution(lv_display_get_default());
  for (uint8_t index = 0;
       index < g_object_count + g_reference_count; ++index) {
    lv_obj_t *object = index < g_reference_count
                           ? g_reference_objects[index]
                           : g_objects[index - g_reference_count];
    lv_area_t area;
    int32_t ext;
    if (object == NULL || lv_obj_is_hidden(object))
      continue;
    bool duplicate = false;
    for (uint8_t previous = 0; previous < count; ++previous)
      if (ordered[previous] == object) duplicate = true;
    if (duplicate) continue;
    lv_obj_get_coords(object, &area);
    ext = lv_obj_get_ext_draw_size(object);
    if (area.x1 - ext < left)
      left = area.x1 - ext;
    if (area.y1 - ext < top)
      top = area.y1 - ext;
    if (area.x2 + ext + 1 > right)
      right = area.x2 + ext + 1;
    if (area.y2 + ext + 1 > bottom)
      bottom = area.y2 + ext + 1;
    ordered[count++] = object;
  }
  if (count == 0) {
    publish_plane(NULL, 0, 0, 0, 0, false);
    return;
  }
  for (uint8_t index = 1; index < count; ++index) {
    lv_obj_t *object = ordered[index];
    uint8_t before = index;
    while (before != 0 &&
           lv_obj_get_parent(ordered[before - 1]) == lv_obj_get_parent(object) &&
           lv_obj_get_index(ordered[before - 1]) > lv_obj_get_index(object)) {
      ordered[before] = ordered[before - 1];
      --before;
    }
    ordered[before] = object;
  }
  if (left < 0)
    left = 0;
  if (top < 0)
    top = 0;
  if (right > display_width)
    right = display_width;
  if (bottom > display_height)
    bottom = display_height;
  if (right <= left || bottom <= top ||
      (size_t)(right - left) * (bottom - top) > PXA_SYSTEM_OVERLAY_MAX_PIXELS)
    goto failed;
  const uint16_t width = (uint16_t)(right - left);
  const uint16_t height = (uint16_t)(bottom - top);
  const size_t pixels = (size_t)width * height;
  memory = heap_caps_calloc(pixels, 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == NULL)
    goto failed;
  colors = (uint16_t *)memory;
  alpha = memory + pixels * sizeof(uint16_t);
  for (uint8_t index = 0; index < count; ++index) {
    lv_obj_t *object = ordered[index];
    lv_area_t area;
    int32_t ext = lv_obj_get_ext_draw_size(object);
    lv_obj_get_coords(object, &area);
    const int32_t source_x = area.x1 - ext;
    const int32_t source_y = area.y1 - ext;
    const int32_t source_width = lv_obj_get_width(object) + 2 * ext;
    const int32_t source_height = lv_obj_get_height(object) + 2 * ext;
    if (source_width <= 0 || source_height <= 0 ||
        (size_t)source_width * source_height > PXA_SYSTEM_OVERLAY_MAX_PIXELS)
      goto failed;
    const uint32_t stride = lv_draw_buf_width_to_stride(
        (uint32_t)source_width, LV_COLOR_FORMAT_ARGB8888);
    const size_t scratch_bytes =
        (size_t)stride * source_height + LV_DRAW_BUF_ALIGN - 1u;
    uint8_t *scratch =
        heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_draw_buf_t draw;
    if (scratch == NULL)
      goto failed;
    const uintptr_t aligned = ((uintptr_t)scratch + LV_DRAW_BUF_ALIGN - 1u) /
                              LV_DRAW_BUF_ALIGN * LV_DRAW_BUF_ALIGN;
    const bool captured =
        lv_draw_buf_init(
            &draw, 1, 1, LV_COLOR_FORMAT_ARGB8888, 0, (void *)aligned,
            (uint32_t)(scratch_bytes - (aligned - (uintptr_t)scratch))) ==
            LV_RESULT_OK &&
        lv_snapshot_take_to_draw_buf(object, LV_COLOR_FORMAT_ARGB8888, &draw) ==
            LV_RESULT_OK;
    if (!captured) {
      heap_caps_free(scratch);
      goto failed;
    }
    for (uint32_t sy = 0; sy < draw.header.h; ++sy) {
      const int32_t dy = source_y + (int32_t)sy - top;
      if (dy < 0 || dy >= height)
        continue;
      const lv_color32_t *source =
          (const lv_color32_t *)((const uint8_t *)draw.data +
                                 (size_t)sy * draw.header.stride);
      for (uint32_t sx = 0; sx < draw.header.w; ++sx) {
        const int32_t dx = source_x + (int32_t)sx - left;
        if (dx < 0 || dx >= width)
          continue;
        const size_t target = (size_t)dy * width + dx;
        blend_pixel(&colors[target], &alpha[target], source[sx]);
      }
    }
    heap_caps_free(scratch);
  }
  publish_plane(memory, left, top, width, height, false);
  return;
failed:
  heap_caps_free(memory);
  ESP_LOGW("PxaOverlay", "System overlay snapshot unavailable");
  publish_plane(NULL, 0, 0, 0, 0, true);
}

void pxa_esp_system_overlay_set_reference_objects(lv_obj_t *const *objects,
                                                   size_t count) {
  if (count > PXA_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS) {
    ESP_LOGW("PxaOverlay", "Too many reference overlay objects");
    count = PXA_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS;
  }
  g_reference_count = (uint8_t)count;
  for (size_t index = 0; index < count; ++index)
    g_reference_objects[index] = objects[index];
  for (size_t index = count;
       index < PXA_SYSTEM_OVERLAY_MAX_REFERENCE_OBJECTS; ++index)
    g_reference_objects[index] = NULL;
  if (g_bound) pxa_esp_system_overlay_refresh();
}

void pxa_esp_system_overlay_add(lv_obj_t *object) {
  if (!g_bound || object == NULL)
    return;
  for (uint8_t index = 0; index < g_object_count; ++index)
    if (g_objects[index] == object)
      return;
  if (g_object_count == PXA_SYSTEM_OVERLAY_MAX_OBJECTS) {
    ESP_LOGW("PxaOverlay", "Too many system overlay objects");
    return;
  }
  g_objects[g_object_count++] = object;
  pxa_esp_system_overlay_refresh();
}

void pxa_esp_system_overlay_remove(lv_obj_t *object) {
  if (!g_bound || object == NULL)
    return;
  for (uint8_t index = 0; index < g_object_count; ++index) {
    if (g_objects[index] != object)
      continue;
    memmove(&g_objects[index], &g_objects[index + 1],
            (size_t)(--g_object_count - index) * sizeof(g_objects[0]));
    g_objects[g_object_count] = NULL;
    pxa_esp_system_overlay_refresh();
    return;
  }
}

#endif
