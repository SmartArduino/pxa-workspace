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
static bool low_space,preview_has_root=true;
static unsigned confirmations, installs;
static bool cancel_after_confirmation;
static pxa_esp_store_job_t *waiting_job;
static const char body[] = "bounded package transport fixture";
void vTaskDelete(TaskHandle_t task) { (void)task; ++deletes; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 512; }
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks) {
    (void)clear; (void)ticks;
    assert(waiting_job != NULL);
    if (cancel_after_confirmation) waiting_job->cancelled = 1;
    waiting_job = NULL;
    return 1;
}
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
    strcpy(preview->app_id, "pxa-store-test");preview->has_publisher_root=preview_has_root; return valid_preview;
}
bool pxa_host_install_package_file(const char *source) {
    (void)source;
    assert(approve && waiting_job == NULL && !cancel_after_confirmation);
    ++installs;
    return install_ok;
}
bool pxa_host_install_package_file_for_identity(const char *source,const uint8_t root[32],const char *app_id) {
    const uint8_t expected[32]={0};assert(!memcmp(root,expected,32) && !strcmp(app_id,"pxa-store-test"));
    return pxa_host_install_package_file(source);
}
static void notified(pxa_esp_store_job_t *job, uint8_t phase) {
    if (phase == 1) {
        assert(job->approved == 0); /* No previous approval can bypass a prompt. */
        ++confirmations;
        waiting_job = job;
        job->approved = approve;
    }
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
    value.approved = 1; /* Even stale approval must be cleared before prompting. */
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_DENIED && !exists(&value));
    assert(confirmations == 1 && installs == 0);
    approve = true;
    value = job(0); value.cancelled = 1;
    pxa_esp_store_worker(&value);
    assert(value.status != PXA_STATUS_OK && !exists(&value));
    assert(confirmations == 1 && installs == 0);
    value = job(0); cancel_after_confirmation = true;
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_DENIED && !exists(&value));
    assert(confirmations == 2 && installs == 0);
    cancel_after_confirmation = false;
    value = job(0);
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_OK && !exists(&value));
    assert(confirmations == 3 && installs == 1);
    value = job(0); /* Reinstall/update must request its own confirmation. */
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_OK && confirmations == 4 && installs == 2);
    value = job(1);
    pxa_esp_store_worker(&value);
    assert(value.status == PXA_STATUS_OK && exists(&value) && saved == 1);
    assert(confirmations == 4 && installs == 2); /* Download alone never installs. */
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
    assert(confirmations == 5 && installs == 2);
    approve = true; install_ok = false;
    pxa_esp_store_worker(&value); assert(!exists(&value));
    assert(confirmations == 6 && installs == 3);
    unsigned before_confirmations=confirmations,before_installs=installs;
    value=job(0);value.has_expected_root=1;value.expected_root[0]=1;
    pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_DENIED && !exists(&value) && confirmations==before_confirmations && installs==before_installs);
    value=job(0);value.has_expected_root=1;preview_has_root=false;
    pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_DENIED && !exists(&value) && confirmations==before_confirmations && installs==before_installs);
    preview_has_root=true;install_ok=true;
    value=job(0);value.has_expected_root=1;
    pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_OK && !exists(&value) && confirmations==before_confirmations+1 && installs==before_installs+1);
    assert(deletes == completions);
    pxa_esp_store_cleanup_interrupted(NULL, NULL);
    assert(rmdir(state) == 0 && rmdir(CONFIG_PXA_MOUNT_POINT) == 0);
    puts("store download: partial/hash failure, cancellation, cache retention, failed install and interrupted cleanup passed");
}
