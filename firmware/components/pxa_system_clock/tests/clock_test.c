#include "test_port.h"

static pxsys_system_status_snapshot_t status(void) {
    pxsys_system_status_snapshot_t value = {.struct_size = sizeof(value)};
    assert(pxa_system_clock_update_status(&value));
    assert(value.time_valid && value.date_valid);
    return value;
}

static void test_default_and_tick(void) {
    reset();
    assert(!pxa_system_clock_save());
    assert(!pxa_system_clock_update_status(NULL));
    assert(pxa_system_clock_initialize());
    pxsys_system_status_snapshot_t value = status();
    assert(value.year == 2026 && value.month == 1 && value.day == 1);
    assert(value.hour == 0 && value.minute == 0 && value.weekday == 4);
    assert(wall_time == INT64_C(1767196800)); /* default is LOCAL midnight */
    assert(wall_sets == 1 && registrations == 1 && shutdown_handler != NULL);
    wall_time += 61;
    value = status();
    assert(value.hour == 0 && value.minute == 1);
    wall_time += 86400;
    value = status();
    assert(value.day == 2 && value.weekday == 5 && value.minute == 1);
    assert(pxa_system_clock_initialize());
    assert(wall_sets == 1 && registrations == 1); /* never rewind on re-entry */
    assert(writes == 0 && commits == 0); /* status polling does not wear flash */

    value.battery_valid = 1;
    value.battery_percent = 73;
    value.volume_supported = 1;
    value.volume_percent = 28;
    value.wifi_connected = 1;
    value.generation = 45;
    const pxsys_system_status_snapshot_t before = value;
    assert(pxa_system_clock_update_status(&value));
    assert(memcmp(&before, &value, sizeof(value)) == 0);
    value.struct_size = 0;
    assert(!pxa_system_clock_update_status(&value));
}

static void test_save_and_restore(void) {
    reset();
    assert(pxa_system_clock_initialize());
    wall_time += 3661;
    const time_t saved = wall_time;
    shutdown_handler(); /* same callback used by esp_restart() */
    assert(stored && stored_utc == saved && writes == 1 && commits == 1);
    reboot();
    assert(pxa_system_clock_initialize());
    assert(wall_time == saved && reads == 1 && closes == 1);
    pxsys_system_status_snapshot_t value = status();
    assert(value.hour == 1 && value.minute == 1);
    wall_time += 60;
    assert(status().minute == 2);
    assert(pxa_system_clock_save()); /* explicit physical power-off */
    assert(stored_utc == saved + 60);

    reboot();
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    assert(pxa_system_clock_initialize());
    value = status();
    assert(value.year == 2025 && value.month == 12 && value.day == 31);
    assert(value.hour == 17 && value.minute == 2); /* persisted UTC, no double offset */
}

typedef struct {
    int64_t utc, last_written;
    bool readable, writable;
    unsigned read_count, write_count;
} fake_rtc_t;

static bool rtc_read(void* context, int64_t* seconds) {
    fake_rtc_t* rtc = context;
    ++rtc->read_count;
    *seconds = rtc->utc;
    return rtc->readable;
}

static bool rtc_write(void* context, int64_t seconds) {
    fake_rtc_t* rtc = context;
    ++rtc->write_count;
    rtc->last_written = seconds;
    return rtc->writable;
}

static void register_rtc(fake_rtc_t* rtc, bool read_only) {
    pxa_system_clock_source_t source = {
        .struct_size = sizeof(source), .context = rtc,
        .read_utc = rtc_read, .write_utc = read_only ? NULL : rtc_write,
    };
    assert(pxa_system_clock_set_source(&source));
    memset(&source, 0, sizeof(source)); /* descriptor is copied */
}

static void test_source_priority_and_correction(void) {
    reset();
    stored = true;
    stored_utc = INT64_C(1780000000);
    wall_time = INT64_C(1790000000);
    fake_rtc_t rtc = {.utc = INT64_C(1800000000), .readable = true, .writable = true};
    register_rtc(&rtc, false);
    assert(pxa_system_clock_initialize());
    assert(wall_time == rtc.utc && rtc.read_count == 1 && reads == 0);
    assert(!pxa_system_clock_set_source(NULL));
    wall_time += 120; /* a manual/network correction of the system clock */
    assert(pxa_system_clock_save());
    assert(rtc.write_count == 1 && rtc.last_written == wall_time);
    assert(stored_utc == wall_time);
    const time_t saved = wall_time;
    reboot();
    rtc.utc = saved + 3600; /* RTC continues through time powered off */
    register_rtc(&rtc, false);
    assert(pxa_system_clock_initialize());
    assert(wall_time == saved + 3600 && reads == 0);

    reboot();
    wall_time = saved + 5; /* on-chip retained system time beats NVS */
    rtc.readable = false;
    register_rtc(&rtc, false);
    assert(pxa_system_clock_initialize());
    assert(wall_time == saved + 5 && wall_sets == 0 && reads == 0);
    reboot();
    register_rtc(&rtc, false); /* failed RTC falls through to NVS */
    assert(pxa_system_clock_initialize());
    assert(wall_time == saved && reads == 1);
    reboot();
    rtc.readable = true;
    rtc.utc = INT64_MAX; /* corrupt hardware time also falls through */
    register_rtc(&rtc, false);
    assert(pxa_system_clock_initialize());
    assert(wall_time == saved);

    reset();
    rtc.utc = saved;
    register_rtc(&rtc, true);
    assert(pxa_system_clock_initialize() && pxa_system_clock_save());
    assert(rtc.write_count == 1); /* read-only source needs no write callback */
}

static void test_invalid_data_and_failures(void) {
    const int64_t bad_times[] = {0, -1, INT64_MIN, INT64_MAX, 1577836799, 253402214400};
    for (size_t i = 0; i < sizeof(bad_times) / sizeof(*bad_times); ++i) {
        reset();
        stored = true;
        stored_utc = bad_times[i];
        wall_time = (time_t)bad_times[i];
        assert(pxa_system_clock_initialize());
        assert(status().year == 2026);
    }
    reset();
    pxa_system_clock_source_t invalid = {.struct_size = sizeof(invalid)};
    assert(!pxa_system_clock_set_source(&invalid));
    invalid.read_utc = rtc_read;
    invalid.struct_size--;
    assert(!pxa_system_clock_set_source(&invalid));
    assert(pxa_system_clock_set_source(NULL));
    fail_wall_set = true;
    assert(!pxa_system_clock_initialize() && shutdown_handler == NULL);
    fail_wall_set = false;
    fail_register = true;
    assert(!pxa_system_clock_initialize());
    fail_register = false;
    assert(pxa_system_clock_initialize());

    fake_rtc_t rtc = {.utc = wall_time, .readable = true, .writable = true};
    for (unsigned failure = 0; failure < 4; ++failure) {
        reboot();
        register_rtc(&rtc, false);
        assert(pxa_system_clock_initialize());
        fail_open = failure == 0;
        fail_set = failure == 1;
        fail_commit = failure == 2;
        rtc.writable = failure != 3;
        unsigned previous_writes = rtc.write_count;
        assert(!pxa_system_clock_save());
        assert(rtc.write_count == previous_writes + 1); /* always try hardware */
        if (failure == 3) assert(commits == 1); /* always try NVS too */
        if (!fail_open) assert(opens == closes);
    }
    reset();
    stored = true;
    stored_utc = INT64_C(1800000000);
    fail_get = true;
    assert(pxa_system_clock_initialize() && status().year == 2026);
    assert(opens == closes);
    wall_time = 0;
    assert(!pxa_system_clock_save());
    assert(!pxa_system_clock_update_status(&(pxsys_system_status_snapshot_t){
        .struct_size = sizeof(pxsys_system_status_snapshot_t)}));
}

int main(void) {
    test_default_and_tick();
    test_save_and_restore();
    test_source_priority_and_correction();
    test_invalid_data_and_failures();
    puts("System clock: default/ticking, UTC persistence/restart, RTC priority/fallback, corrections and I/O failures passed");
    return 0;
}
