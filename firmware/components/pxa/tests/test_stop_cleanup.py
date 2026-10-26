#!/usr/bin/env python3
"""Run real activation teardown with a system callback that starts new work."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "firmware/components/pxa_integration/tests"))
from test_startup import function

FIXTURE = r'''
#include "pxa/runtime.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#define PXA_ESP_HOST_MAX_APP_ID 130
#define PXA_HOST_RUNTIME_STOPPED 7
#define PXA_ESP_HOST_TAG "test"
#define ESP_LOGW(...) unused(__VA_ARGS__)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
struct pxa_esp_host_public_state_t { bool main_active; uint64_t main_instance_id; } published;
struct Services { void *window_workspace, *window; };
struct Activation {
    Services services;
    void *coordinator, *coordinator_workspace, *plan_workspace;
    void *active_manifest, *manifest_workspace_keep, *encoded_keep;
    void *active_components, *active_jobs, *job_stop_at_ms, *job_force_stop_at_ms;
    void *active_work, *work_result, *runtime, *app_storage;
    char active_package_root[32], active_identity[130], active_name[32];
    pxa_component_t active_component, ui_component;
    uint64_t active_instance_id, package_instance_id;
    unsigned active_component_count, active_component_capacity;
    int activating_job_slot;
};
struct Host {
    Activation activation;
    void *engine, *ui_adapter, *engine_workspace, *runtime_workspace, *retired_storage;
    unsigned graceful_stop_active, ui_foreground_requested, lifecycle_effective;
    unsigned lifecycle_delivered, ime_only_runtime;
    char deferred_launch_identity[130];
} g_host;
struct Job {
    uint64_t instance_id, prompt_id, request_token;
    pxa_component_t component;
    unsigned cancelled, approved, separate, ready;
    void *worker;
} *g_store_install, g_store_uninstall;
struct { unsigned prompt_id; } g_store_result;
enum { ENGINE=1, RUNTIME=2, AUDIO=4, SERVICES=8, PERMISSIONS=16, RESOURCE=32,
       NET=64, STORAGE=128, ENGINE_WORK=256, RUNTIME_WORK=512, ARENA=1024, CLOCKS=2048 };
unsigned live, notifications, closed_storage, resources_retired;
bool defer_storage, start_in_callback, new_work;
pxa_stop_reason_t expected_reason;
static void *token(unsigned value) { return reinterpret_cast<void *>(uintptr_t(value)); }
static void retire(unsigned flag) { assert(!new_work); live &= ~flag; }
template <typename... A> static void unused(A...) {}
#define pxa_esp_ime_suspend unused
#define pxa_esp_ime_stop unused
#define pxa_esp_ime_release unused
#define pxa_esp_ui_shell_dismiss_install_prompt unused
#define pxa_esp_ui_shell_dismiss_permission_prompt unused
#define pxa_esp_ui_shell_dismiss_store_result unused
#define pxa_esp_surface_runtime_modal_leave unused
#define xTaskNotifyGive unused
#define pxa_request_cancel unused
#define dismiss_runtime_permission_prompt unused
#define dismiss_unresponsive_prompt unused
#define host_log_ui_usage unused
#define host_log_audio_usage unused
#define host_log_net_usage unused
#define host_log_runtime_usage unused
#define host_log_wamr_usage unused
#define host_log_activation_memory_usage unused
#define pxa_activation_deactivate_all unused
#define pxa_lvgl_ui_reset unused
#define pxa_esp_ui_assets_clear unused
#define clear_pointer_mailbox unused
#define rearm_clock_timer unused
#define pxa_wamr_engine_set_runtime unused
#define pxa_host_clock_slots_cancel_all(...) retire(CLOCKS)
static void public_state_snapshot(pxa_esp_host_public_state_t *state) { *state=published; }
static void unpublish_active_state() { published={}; }
static void pxa_wamr_engine_deinit(void *value) { assert(value==token(ENGINE)); retire(ENGINE); }
static void pxa_runtime_deinit(void *value) { assert(value==token(RUNTIME)); retire(RUNTIME); }
static void pxa_esp_audio_deinitialize() { retire(AUDIO); }
static void destroy_services() { retire(SERVICES); }
static void pxa_esp_permission_store_release_cache() { retire(PERMISSIONS); }
static int pxa_esp_resource_memory_end() { ++resources_retired; retire(RESOURCE); return 0; }
static void pxa_esp_net_reset_requests() { retire(NET); }
static pxa_status_t pxa_esp_app_storage_close(void **storage) {
    assert(*storage==token(STORAGE)); ++closed_storage; retire(STORAGE);
    if (!defer_storage) *storage=nullptr;
    return defer_storage ? PXA_STATUS_IO_ERROR : PXA_STATUS_OK;
}
static void test_free(void *value) {
    if (value==token(ENGINE_WORK)) retire(ENGINE_WORK);
    else if (value==token(RUNTIME_WORK)) retire(RUNTIME_WORK);
    else assert(!value);
}
static void activation_memory_release_all() { retire(ARENA); }
static void activation_state_reset() { g_host.activation={}; }
static void notify_runtime_event(unsigned event, const char *identity,
                                 uint64_t instance, unsigned reason) {
    assert(event==PXA_HOST_RUNTIME_STOPPED && !strcmp(identity,"publisher:old"));
    assert(instance==42 && reason==unsigned(expected_reason));
    /* A launcher rebuild allocates persistent icons. It must see all old
     * guest/code/workspace storage retired, even on fault/replacement exits. */
    assert(!live && !published.main_active && resources_retired && closed_storage);
    assert(!g_host.engine && !g_host.activation.runtime && !g_host.activation.app_storage);
    assert(!g_host.engine_workspace && !g_host.runtime_workspace);
    assert(!g_host.activation.active_identity[0]);
    assert(g_host.retired_storage==(defer_storage ? token(STORAGE) : nullptr));
    ++notifications;
    if (start_in_callback) { new_work=true; g_host.engine=token(4096); }
}
#define free test_free
// PRODUCTION
#undef free
static void reset(bool active) {
    g_host={};published={active,42};g_store_install=nullptr;g_store_uninstall={};g_store_result={};
    g_host.activation.active_component=7;
    g_host.activation.active_instance_id=42;
    strcpy(g_host.activation.active_identity,"publisher:old");
    g_host.activation.runtime=token(RUNTIME);g_host.activation.app_storage=token(STORAGE);
    g_host.activation.coordinator=token(4095);
    g_host.engine=token(ENGINE);g_host.engine_workspace=token(ENGINE_WORK);
    g_host.runtime_workspace=token(RUNTIME_WORK);
    live=4095;notifications=closed_storage=resources_retired=0;
    defer_storage=start_in_callback=new_work=false;
}
int main() {
    for (auto reason : {PXA_STOP_NORMAL,PXA_STOP_FAULT,PXA_STOP_REPLACED,PXA_STOP_SHUTDOWN}) {
        reset(true);expected_reason=reason;start_in_callback=true;
        stop_active(reason);
        assert(notifications==1 && new_work && g_host.engine==token(4096));
    }
    reset(true);expected_reason=PXA_STOP_NORMAL;defer_storage=true;
    stop_active(expected_reason);assert(notifications==1 && !live);
    reset(false);stop_active(PXA_STOP_NORMAL);
    assert(!notifications && !live && closed_storage==1);
    stop_active(PXA_STOP_NORMAL);assert(!notifications && !live && closed_storage==1);
    puts("Activation stop: notification follows guest/service/storage/arena retirement; callback reentry, inactive repeat and deferred storage passed");
}
'''


def main():
    production = (ROOT / "firmware/components/pxa/src/runtime/pxa_esp_host.c").read_text()
    source = FIXTURE.replace("// PRODUCTION", function(production, "void stop_active(pxa_stop_reason_t reason) {"))
    with tempfile.TemporaryDirectory(prefix="pxa-stop-cleanup-") as directory:
        unit = Path(directory) / "stop.cpp"
        binary = Path(directory) / "stop"
        unit.write_text(source)
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra",
                        "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I" + str(ROOT / "deps/pxa-system/libpxa/include"),
                        str(unit), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
