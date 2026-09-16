#include "pxa_esp_package_policy.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "pxa_esp_package_store.h"
#include "pxa_package_disabled_policy.h"

#define PXA_ESP_PACKAGE_POLICY_TAG "PxaPackagePolicy"
#define PXA_ESP_PACKAGE_POLICY_NS "pxa_apps"
#define PXA_ESP_PACKAGE_POLICY_KEY "disabled"
#define PXA_ESP_PACKAGE_POLICY_GROWTH 4u

typedef char pxa_esp_package_policy_id_size_must_match[
    PXA_ESP_PACKAGE_ID_BYTES == PXA_PACKAGE_DISABLED_POLICY_ID_BYTES ? 1 : -1];

static pxa_package_disabled_policy_t g_policy;
static bool g_initialized;

static void *policy_reallocate(void *context, void *memory, size_t size) {
    void *resized;
    (void)context;
    if (memory == NULL) {
        resized = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (resized == NULL) {
            resized = heap_caps_malloc(size, MALLOC_CAP_8BIT);
        }
        return resized;
    }
    resized = heap_caps_realloc(memory, size,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resized == NULL) {
        resized = heap_caps_realloc(memory, size, MALLOC_CAP_8BIT);
    }
    return resized;
}

static void policy_release(void *context, void *memory) {
    (void)context;
    heap_caps_free(memory);
}

static bool persist_policy(void *context, const uint8_t *bytes, size_t size) {
    nvs_handle_t handle;
    esp_err_t status;
    (void)context;
    if (nvs_open(PXA_ESP_PACKAGE_POLICY_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    if (size == 0) {
        status = nvs_erase_key(handle, PXA_ESP_PACKAGE_POLICY_KEY);
        if (status == ESP_ERR_NVS_NOT_FOUND) status = ESP_OK;
    } else {
        status = nvs_set_blob(handle, PXA_ESP_PACKAGE_POLICY_KEY, bytes, size);
    }
    if (status == ESP_OK) status = nvs_commit(handle);
    nvs_close(handle);
    return status == ESP_OK;
}

bool pxa_esp_package_policy_initialize(void) {
    pxa_package_disabled_policy_config_t config;
    nvs_handle_t handle;
    uint8_t *bytes = NULL;
    size_t size = 0;
    esp_err_t status;
    bool loaded;
    if (g_initialized) return true;
    config = (pxa_package_disabled_policy_config_t){
        .max_entries = SIZE_MAX,
        .growth = PXA_ESP_PACKAGE_POLICY_GROWTH,
        .reallocate = policy_reallocate,
        .release = policy_release,
        .persist = persist_policy,
    };
    if (!pxa_package_disabled_policy_init(&g_policy, &config)) return false;
    status = nvs_open(PXA_ESP_PACKAGE_POLICY_NS, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        g_initialized = true;
        return true;
    }
    if (status != ESP_OK) {
        ESP_LOGE(PXA_ESP_PACKAGE_POLICY_TAG,
                 "Open disabled-policy NVS failed: status=0x%x",
                 (unsigned)status);
        goto failed;
    }
    status = nvs_get_blob(handle, PXA_ESP_PACKAGE_POLICY_KEY, NULL, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND || size == 0) {
        nvs_close(handle);
        g_initialized = true;
        return true;
    }
    if (status != ESP_OK) {
        nvs_close(handle);
        goto failed;
    }
    bytes = policy_reallocate(NULL, NULL, size);
    if (bytes == NULL) {
        nvs_close(handle);
        goto failed;
    }
    status = nvs_get_blob(handle, PXA_ESP_PACKAGE_POLICY_KEY, bytes, &size);
    nvs_close(handle);
    if (status != ESP_OK) goto failed;
    loaded = pxa_package_disabled_policy_load(&g_policy, bytes, size);
    policy_release(NULL, bytes);
    if (!loaded) goto failed_without_bytes;
    g_initialized = true;
    return true;

failed:
    policy_release(NULL, bytes);
failed_without_bytes:
    pxa_package_disabled_policy_deinit(&g_policy);
    return false;
}

void pxa_esp_package_policy_deinitialize(void) {
    pxa_package_disabled_policy_deinit(&g_policy);
    g_initialized = false;
}

bool pxa_esp_package_policy_is_disabled(const char *identity) {
    return g_initialized &&
           pxa_package_disabled_policy_contains(&g_policy, identity);
}

bool pxa_esp_package_policy_set_enabled(const char *identity, bool enabled) {
    return g_initialized && pxa_package_disabled_policy_set_enabled(
                                &g_policy, identity, enabled);
}

#endif
