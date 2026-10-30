#include "../src/esp_mosaico_haptics.cc"
#include <cstdio>

static pxa_haptics_backend_t backend{};
extern "C" bool pxa_esp_haptics_set_backend(const pxa_haptics_backend_t* value) {
    backend = *value; return true;
}
int main() {
    assert(EspMosaicoVibrate(20)==ESP_ERR_INVALID_STATE);
    assert(EspMosaicoInitializeHaptics() && !test_motor_level);
    assert(EspMosaicoVibrate(1001)==ESP_ERR_INVALID_ARG && !test_motor_level);
    assert(EspMosaicoVibrate(100)==ESP_OK && test_motor_level);
    test_now_us=50000;
    assert(EspMosaicoVibrate(200)==ESP_OK);
    test_now_us=100000;test_timer.callback(nullptr); // previous queued expiry
    assert(test_motor_level && test_timer.due_us==250000);
    test_now_us=250000;test_timer.callback(nullptr);
    assert(!test_motor_level);
    assert(EspMosaicoVibrate(1000)==ESP_OK && test_motor_level);
    assert(EspMosaicoVibrate(0)==ESP_OK && !test_motor_level);
    test_timer_fail=true;
    assert(EspMosaicoVibrate(10)!=ESP_OK && !test_motor_level);
    test_timer_fail=false;
    assert(backend.flags == PXA_HAPTICS_CAP_VIBRATE && backend.max_duration_ms == 1000);
    assert(backend.play(nullptr, 20, 500) == PXA_STATUS_UNSUPPORTED);
    assert(backend.play(nullptr, 100, 1000) == PXA_STATUS_OK && test_motor_level);
    test_timer_fail = true;
    test_timer.callback(nullptr); // failed rearm of a stale callback must stop.
    assert(!test_motor_level);
    test_timer_fail = false;
    assert(backend.play(nullptr, 20, 1000) == PXA_STATUS_OK);
    backend.stop(nullptr); assert(!test_motor_level);
    puts("Haptics: duration bound, replacement, stale callback, cancellation, failed timer and standard backend passed");
}
