#include "pxa_system_clock.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static pxa_system_clock_source_t g_source;
static bool g_initialized;

static bool valid_time(int64_t seconds) {
    /* Match the supported calendar range, rejecting erased/corrupt RTC/NVS. */
    return seconds >= INT64_C(1577836800) &&
           seconds <= INT64_C(253402214399); /* through 9999-12-30 UTC */
}

bool pxa_system_clock_set_source(const pxa_system_clock_source_t* source) {
    if (g_initialized ||
        (source != NULL && (source->struct_size != sizeof(*source) ||
                            source->read_utc == NULL)))
        return false;
    if (source != NULL)
        g_source = *source;
    else
        memset(&g_source, 0, sizeof(g_source));
    return true;
}

static bool load_saved_time(int64_t* seconds) {
    nvs_handle_t handle;
    if (nvs_open("pxa_clock", NVS_READONLY, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_get_i64(handle, "utc", seconds);
    nvs_close(handle);
    return result == ESP_OK && valid_time(*seconds);
}

static void save_on_restart(void) {
    (void)pxa_system_clock_save();
}

bool pxa_system_clock_initialize(void) {
    if (g_initialized) return true;
    int64_t seconds = 0;
    const time_t retained = time(NULL);
    const bool from_source = g_source.read_utc != NULL &&
        g_source.read_utc(g_source.context, &seconds) && valid_time(seconds);
    if (!from_source) {
        if (valid_time((int64_t)retained)) {
            seconds = (int64_t)retained;
        } else if (!load_saved_time(&seconds)) {
            struct tm initial = {
                .tm_year = 2026 - 1900, .tm_mon = 0, .tm_mday = 1,
                .tm_isdst = -1,
            };
            seconds = (int64_t)mktime(&initial);
        }
    }
    if (!valid_time(seconds)) return false;
    /* Leave a retained/system-synchronized clock untouched, including its
     * subsecond part. A cold boot or registered RTC needs to seed libc time. */
    if (from_source || !valid_time((int64_t)retained)) {
        const struct timeval value = {.tv_sec = (time_t)seconds, .tv_usec = 0};
        if ((int64_t)value.tv_sec != seconds || settimeofday(&value, NULL) != 0) {
            ESP_LOGE("pxa_clock", "Could not restore system time");
            return false;
        }
    }
    const esp_err_t result = esp_register_shutdown_handler(save_on_restart);
    if (result != ESP_OK) {
        ESP_LOGE("pxa_clock", "Could not register clock shutdown handler: %d", (int)result);
        return false;
    }
    g_initialized = true;
    return true;
}

bool pxa_system_clock_update_status(pxsys_system_status_snapshot_t* status) {
    if (!g_initialized || status == NULL || status->struct_size < sizeof(*status))
        return false;
    const time_t now = time(NULL);
    struct tm local;
    if (!valid_time((int64_t)now) || localtime_r(&now, &local) == NULL)
        return false;
    status->time_valid = status->date_valid = 1;
    status->hour = (uint8_t)local.tm_hour;
    status->minute = (uint8_t)local.tm_min;
    status->year = (uint16_t)(local.tm_year + 1900);
    status->month = (uint8_t)(local.tm_mon + 1);
    status->day = (uint8_t)local.tm_mday;
    status->weekday = (uint8_t)local.tm_wday;
    return true;
}

bool pxa_system_clock_save(void) {
    if (!g_initialized) return false;
    const int64_t seconds = (int64_t)time(NULL);
    if (!valid_time(seconds)) return false;
    /* Try both stores even if one fails, so a peripheral or flash error does
     * not discard the other available recovery path. */
    const bool source_saved = g_source.write_utc == NULL ||
        g_source.write_utc(g_source.context, seconds);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("pxa_clock", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_i64(handle, "utc", seconds);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK || !source_saved)
        ESP_LOGW("pxa_clock", "Could not save clock: NVS=%d RTC=%d",
                 (int)result, (int)source_saved);
    return result == ESP_OK && source_saved;
}
