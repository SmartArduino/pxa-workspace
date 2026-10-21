/* Exercise the production service with deterministic wall time, NVS and RTC.
 * Including it lets reboot() reset RAM state without a production reset API. */
#ifndef PXA_SYSTEM_CLOCK_TEST_PORT_H
#define PXA_SYSTEM_CLOCK_TEST_PORT_H

#define time clock_test_time
#define settimeofday clock_test_settimeofday
#include "../pxa_system_clock.c"
#undef time
#undef settimeofday

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static time_t wall_time;
static bool stored, fail_open, fail_get, fail_set, fail_commit;
static bool fail_wall_set, fail_register;
static int64_t stored_utc, pending_utc;
static unsigned reads, writes, commits, opens, closes, wall_sets, registrations;
static void (*shutdown_handler)(void);

time_t clock_test_time(time_t* output) {
    if (output != NULL) *output = wall_time;
    return wall_time;
}

int clock_test_settimeofday(const struct timeval* value, const struct timezone* zone) {
    assert(zone == NULL && value->tv_usec == 0);
    ++wall_sets;
    if (fail_wall_set) return -1;
    wall_time = value->tv_sec;
    return 0;
}

esp_err_t esp_register_shutdown_handler(void (*handler)(void)) {
    ++registrations;
    if (fail_register) return ESP_FAIL;
    assert(shutdown_handler == NULL);
    shutdown_handler = handler;
    return ESP_OK;
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    assert(strcmp(name, "pxa_clock") == 0);
    ++opens;
    if (fail_open || (mode == NVS_READONLY && !stored)) return ESP_FAIL;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_i64(nvs_handle_t handle, const char* key, int64_t* value) {
    assert(handle == 1 && strcmp(key, "utc") == 0);
    ++reads;
    if (fail_get || !stored) return ESP_FAIL;
    *value = stored_utc;
    return ESP_OK;
}

esp_err_t nvs_set_i64(nvs_handle_t handle, const char* key, int64_t value) {
    assert(handle == 1 && strcmp(key, "utc") == 0);
    ++writes;
    if (fail_set) return ESP_FAIL;
    pending_utc = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1);
    ++commits;
    if (fail_commit) return ESP_FAIL;
    stored_utc = pending_utc;
    stored = true;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    assert(handle == 1);
    ++closes;
}

static void reboot(void) {
    g_initialized = false;
    memset(&g_source, 0, sizeof(g_source));
    wall_time = 0;
    shutdown_handler = NULL;
    reads = writes = commits = opens = closes = wall_sets = registrations = 0;
    fail_open = fail_get = fail_set = fail_commit = false;
    fail_wall_set = fail_register = false;
}

static void reset(void) {
    reboot();
    stored = false;
    stored_utc = pending_utc = 0;
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
}

#endif
