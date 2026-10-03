# LVGL allocator that keeps LVGL's objects out of internal RAM. Boards opt in
# by selecting CONFIG_LV_USE_STDLIB_MALLOC=255 (LV_STDLIB_CUSTOM).
idf_component_register(
    SRCS "src/lvgl_psram_alloc.c"
    INCLUDE_DIRS "include")
