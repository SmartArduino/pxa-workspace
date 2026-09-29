/* Render a 64x64 straight-BGRA PXR1 UI icon at its final size with LVGL 9.6.
 * Output is raw premultiplied BGRA for PXR1 encoding 8. Keep the LVGL build
 * version identical to the firmware: antialiasing is version-sensitive. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 6
#error "Weather premultiplied icons must be generated with LVGL 9.6"
#endif

static unsigned read_u16(const uint8_t *bytes) {
    return (unsigned)bytes[0] | (unsigned)bytes[1] << 8;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: compile-lvgl-premul-icon SOURCE.pxr OUTPUT.bgra SIZE\n");
        return 2;
    }
    char *end = NULL;
    unsigned long side = strtoul(argv[3], &end, 10);
    if (!end || *end || side < 1 || side > 256) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 2;
    uint8_t header[32];
    uint8_t source[64 * 64 * 4];
    int valid = fread(header, 1, sizeof(header), input) == sizeof(header) &&
                memcmp(header, "PXR1", 4) == 0 &&
                read_u16(header + 4) == 1 && read_u16(header + 6) == 0 &&
                read_u16(header + 8) == 4 && read_u16(header + 10) == 7 &&
                read_u16(header + 12) == 64 && read_u16(header + 14) == 64 &&
                header[16] == 0 && header[17] == 64 &&
                header[20] == 32 && header[21] == 0 &&
                fread(source, 1, sizeof(source), input) == sizeof(source) &&
                fgetc(input) == EOF;
    if (fclose(input) != 0 || !valid) return 2;
    uint8_t *result = calloc((size_t)side * side, 4);
    if (!result) return 1;
    lv_init();
    lv_draw_image_dsc_t draw;
    lv_draw_image_dsc_init(&draw);
    draw.scale_x = draw.scale_y = (uint16_t)(side * 256 / 64);
    draw.pivot.x = draw.pivot.y = 0;
    draw.antialias = 1;
    lv_area_t area = {0, 0, (int32_t)side - 1, (int32_t)side - 1};
    lv_draw_sw_transform(&area, source, 64, 64, 64 * 4, &draw, NULL,
                         LV_COLOR_FORMAT_ARGB8888, result);
    for (size_t i = 0; i < (size_t)side * side * 4; i += 4) {
        if (result[i] > result[i + 3] || result[i + 1] > result[i + 3] ||
            result[i + 2] > result[i + 3]) {
            fprintf(stderr, "LVGL did not emit premultiplied BGRA\n");
            lv_deinit();
            free(result);
            return 1;
        }
    }
    FILE *output = fopen(argv[2], "wb");
    int ok = output && fwrite(result, 4, (size_t)side * side, output) ==
                           (size_t)side * side;
    if (output && fclose(output) != 0) ok = 0;
    lv_deinit();
    free(result);
    return ok ? 0 : 1;
}
