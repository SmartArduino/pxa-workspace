#define _POSIX_C_SOURCE 200809L
#define ESP_PLATFORM 1
#define CONFIG_PXA_STATE_ROOT "state"
#define CONFIG_PXA_STORAGE_PARTITION_LABEL "test"
#include "pxa_test_platform.h"
#include "pxa_esp_store_download.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mbedtls/md.h>

typedef uint32_t StackType_t;
static esp_http_client_config_t http;
static unsigned random_id, deletes, completions, saved;
static bool short_body, valid_preview = true, approve = true, install_ok = true;
static bool low_space;
static const char body[] = "bounded package transport fixture";
void vTaskDelete(TaskHandle_t task) { (void)task; ++deletes; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 512; }
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks) { (void)clear; (void)ticks; return 1; }
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t status) { (void)status; return "test"; }
uint32_t esp_random(void) { return ++random_id; }
void esp_crt_bundle_attach(void) {}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    http = *config; return &http;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
    (void)client;
    esp_http_client_event_t event = {0};
    event.user_data = http.user_data;
    event.event_id = HTTP_EVENT_ON_DATA;
    event.data = (void *)body;
    event.data_len = (int)sizeof(body) - (short_body ? 2 : 0);
    return http.event_handler(&event);
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { (void)client; return 200; }
void esp_http_client_cleanup(esp_http_client_handle_t client) { (void)client; }
esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used) {
    (void)label; *total = 9000000; *used = low_space ? 8999999 : 1000000; return ESP_OK;
}
bool pxa_esp_package_store_preview_file(const char *source, pxa_esp_package_preview_t *preview) {
    (void)source; memset(preview, 0, sizeof(*preview));
    strcpy(preview->app_id, "pxa-store-test"); return valid_preview;
}
bool pxa_host_install_package_file(const char *source) { (void)source; return install_ok; }
static void notified(pxa_esp_store_job_t *job, uint8_t phase) {
    if (phase == 1) job->approved = approve;
    if (phase == 2) ++completions;
    if (phase == 3) ++saved;
}

/* Exercise the actual worker and filesystem, replacing only HTTP/RTOS/Host
 * boundaries. Signature and transaction validation use the real adapter tests. */
#include "../src/package/pxa_esp_store_download.c"

static pxa_esp_store_job_t job(unsigned separate) {
    pxa_esp_store_job_t value = {0};
    value.separate = (uint8_t)separate;
    value.size = sizeof(body);
    value.notify = notified;
    strcpy(value.app_id, "pxa-store-test");
    assert(mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                      (const unsigned char *)body, sizeof(body), value.digest) == 0);
    return value;
}
static bool exists(const pxa_esp_store_job_t *value) {
    char path[256];
    snprintf(path, sizeof(path), "%s/state/%s", CONFIG_PXA_MOUNT_POINT, value->filename);
    return access(path, F_OK) == 0;
}
static int keep(const char *name, void *context) { return strcmp(name, context) == 0; }
int main(void) {
    char state[256];
    assert(mkdir(CONFIG_PXA_MOUNT_POINT, 0700) == 0);
    snprintf(state, sizeof(state), "%s/state", CONFIG_PXA_MOUNT_POINT);
    assert(mkdir(state, 0700) == 0);
    pxa_esp_store_job_t value = job(1);
    low_space = true;
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_QUOTA_EXCEEDED && value.filename[0] == '\0');
    low_space = false;
    value = job(1);
    short_body = true;
    pxa_esp_store_worker(&value);
    assert(value.status != PXA_STATUS_OK && !exists(&value));
    short_body = false;
    value = job(1); value.digest[0] ^= 1;
    pxa_esp_store_worker(&value);
    assert(value.status != PXA_STATUS_OK && !exists(&value));
    value = job(0); approve = false;
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_DENIED && !exists(&value));
    approve = true;
    value = job(0); value.cancelled = 1;
    pxa_esp_store_worker(&value);
    assert(value.status != PXA_STATUS_OK && !exists(&value));
    value = job(0);
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_OK && !exists(&value));
    value = job(1);
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_OK && exists(&value) && saved == 1);
    pxa_esp_store_job_t orphan = job(1);
    pxa_esp_store_worker(&orphan);
    assert(exists(&orphan));
    pxa_esp_store_job_t orphan2 = job(1), orphan3 = job(1);
    pxa_esp_store_worker(&orphan2);
    pxa_esp_store_worker(&orphan3);
    pxa_esp_store_cleanup_interrupted(keep, value.filename);
    assert(exists(&value) && !exists(&orphan) && !exists(&orphan2) && !exists(&orphan3));
    value.separate = 2; valid_preview = false;
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_DENIED && !exists(&value));
    valid_preview = true;
    value = job(1); pxa_esp_store_worker(&value); assert(exists(&value));
    value.separate = 2; approve = false;
    pxa_esp_store_worker(&value); assert(exists(&value));
    approve = true; install_ok = false;
    pxa_esp_store_worker(&value); assert(!exists(&value));
    assert(deletes == completions);
    pxa_esp_store_cleanup_interrupted(NULL, NULL);
    assert(rmdir(state) == 0 && rmdir(CONFIG_PXA_MOUNT_POINT) == 0);
    puts("store download: partial/hash failure, cancellation, cache retention, failed install and interrupted cleanup passed");
}
