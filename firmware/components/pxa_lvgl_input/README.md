# Serialize manual LVGL input

The managed ESP LVGL port processes wake-up input outside `lv_timer_handler`,
under its own display mutex. PXA dispatches events and deletes objects under
LVGL’s global mutex. Concurrent dispatch can corrupt LVGL’s global event stack
with pointers into another task’s stack, later crashing `lv_event_mark_deleted`.

PXA integration explicitly depends on this component, including in ESP-IDF
minimal builds. Link wrapping takes the existing recursive LVGL mutex around
`lv_indev_read`. Timer dispatch can recurse safely; the wrapper creates no
allocation, cache, task or mutex. The managed dependency is unchanged.

Run `bash tools/test-lvgl-input-lock.sh` after building the pai-touch simulator.
The test uses the actual LVGL library, exercises recursive timer reads, and
races 3000 manual reads against 1500 Host event/create/delete operations.
The protected build passes; linking the same test without wrapping reproduces
the overlapping event callback assertion. Device checks cover audio permission
prompts, gameplay, repeated starts, menus, and application stop.
