#include "pxa_esp_store_download.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_littlefs.h"
#include "mbedtls/md.h"
#include "pxa/pxa_host.h"
#include "pxa/status.h"

#ifndef CONFIG_PXA_STORE_ORIGIN
#define CONFIG_PXA_STORE_ORIGIN "https://app.doit.am"
#endif

typedef struct {
    FILE *file;
    pxa_esp_store_job_t *job;
    mbedtls_md_context_t sha;
    uint64_t received;
    uint64_t expected;
    int failed;
} download_sink_t;

void pxa_esp_store_cleanup_interrupted(int (*keep)(const char *filename, void *context),
                                       void *context) {
    const char root[] = CONFIG_PXA_MOUNT_POINT "/" CONFIG_PXA_STATE_ROOT;
    static const char prefix[] = ".store-";
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        char path[160];
        struct stat metadata;
        const char *name = entry->d_name;
        size_t index;
        if (strncmp(name, prefix, sizeof(prefix) - 1u) != 0 ||
            strlen(name) != sizeof(prefix) - 1u + 8u + 4u) continue;
        for (index = sizeof(prefix) - 1u; index < sizeof(prefix) - 1u + 8u;
             ++index) {
            if (!((name[index] >= '0' && name[index] <= '9') ||
                  (name[index] >= 'a' && name[index] <= 'f'))) break;
        }
        if (index != sizeof(prefix) - 1u + 8u ||
            strcmp(name + index, ".pxa") != 0 ||
            snprintf(path, sizeof(path), "%s/%s", root, name) >=
                (int)sizeof(path) || stat(path, &metadata) != 0 ||
            !S_ISREG(metadata.st_mode)) continue;
        if (keep != NULL && keep(name, context)) continue;
        (void)unlink(path);
    }
    closedir(directory);
}

int pxa_esp_store_job_decode(pxa_esp_store_job_t *job,
                             const uint8_t *payload, size_t size) {
    static const char prefix[] = "/api/v2/artifacts/";
    static const char suffix[] = "/download?expires=";
    static const char signature[] = "&signature=";
    size_t offset;
    size_t app_size;
    size_t ticket_size;
    if (job == NULL || payload == NULL || size < 43u) return 0;
    app_size = payload[0];
    ticket_size = (size_t)payload[1] | ((size_t)payload[2] << 8);
    if (app_size == 0 || app_size > 64 || ticket_size >= sizeof(job->ticket) ||
        size != 43u + app_size + ticket_size) return 0;
    job->size = 0;
    for (size_t index = 0; index < 8u; ++index)
        job->size |= (uint64_t)payload[3u + index] << (index * 8u);
    if (job->size == 0 || job->size > PXA_STORE_DOWNLOAD_MAX_BYTES) return 0;
    memcpy(job->digest, payload + 11u, 32u);
    memcpy(job->app_id, payload + 43u, app_size);
    job->app_id[app_size] = '\0';
    for (size_t index = 0; index < app_size; ++index) {
        const char byte = job->app_id[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
              byte == '.' || byte == '_' || byte == '-')) return 0;
    }
    memcpy(job->ticket, payload + 43u + app_size, ticket_size);
    job->ticket[ticket_size] = '\0';
    if (memcmp(job->ticket, prefix, sizeof(prefix) - 1u) != 0) return 0;
    offset = sizeof(prefix) - 1u;
    if (job->ticket[offset] < '0' || job->ticket[offset] > '9') return 0;
    while (job->ticket[offset] >= '0' && job->ticket[offset] <= '9') ++offset;
    if (strncmp(job->ticket + offset, suffix, sizeof(suffix) - 1u) != 0) return 0;
    offset += sizeof(suffix) - 1u;
    if (job->ticket[offset] < '0' || job->ticket[offset] > '9') return 0;
    while (job->ticket[offset] >= '0' && job->ticket[offset] <= '9') ++offset;
    if (strncmp(job->ticket + offset, signature, sizeof(signature) - 1u) != 0)
        return 0;
    offset += sizeof(signature) - 1u;
    if (ticket_size - offset != 64u) return 0;
    for (; offset < ticket_size; ++offset) {
        const char byte = job->ticket[offset];
        if (!((byte >= 'a' && byte <= 'f') || (byte >= '0' && byte <= '9')))
            return 0;
    }
    return 1;
}

static esp_err_t download_event(esp_http_client_event_t *event) {
    download_sink_t *sink = (download_sink_t *)event->user_data;
    size_t length;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
        return ESP_OK;
    length = (size_t)event->data_len;
    if (sink->failed || length > sink->expected - sink->received ||
        fwrite(event->data, 1, length, sink->file) != length ||
        mbedtls_md_update(&sink->sha, event->data, length) != 0) {
        sink->failed = 1;
        return ESP_FAIL;
    }
    sink->received += length;
    if (sink->job != NULL && sink->expected != 0u) {
        uint8_t percent = (uint8_t)(sink->received * 100u / sink->expected);
        if (percent >= sink->job->reported_percent + 5u ||
            sink->received == sink->expected) {
            sink->job->downloaded_bytes = sink->received;
            sink->job->reported_percent = percent;
            sink->job->notify(sink->job, 4u);
        }
    }
    return ESP_OK;
}

void pxa_esp_store_worker(void *context) {
    pxa_esp_store_job_t *job = (pxa_esp_store_job_t *)context;
    download_sink_t sink = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char path[160];
    char url[512];
    uint8_t digest[32];
    int descriptor = -1;
    int downloaded = 0;
    esp_err_t http_result = ESP_FAIL;
    const char *origin = CONFIG_PXA_STORE_ORIGIN;
    size_t origin_size = strlen(origin);
    job->status = PXA_STATUS_IO_ERROR;
    if (origin_size < 9u || strncmp(origin, "https://", 8u) != 0 ||
        strchr(origin + 8u, '/') != NULL || strchr(origin + 8u, '?') != NULL ||
        origin_size + strlen(job->ticket) >= sizeof(url) ||
        snprintf(url, sizeof(url), "%s%s", origin, job->ticket) >= (int)sizeof(url))
        goto done;
    if (job->separate == 2u) {
        if (snprintf(path, sizeof(path), "%s/%s/%s", CONFIG_PXA_MOUNT_POINT,
                     CONFIG_PXA_STATE_ROOT, job->filename) >= (int)sizeof(path)) goto done;
        job->keep_file = 1;
    } else {
        if (snprintf(path, sizeof(path), "%s/%s/.store-%08lx.pxa",
                     CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT,
                     (unsigned long)esp_random()) >= (int)sizeof(path)) goto done;
        snprintf(job->filename, sizeof(job->filename), "%s", strrchr(path, '/') + 1);
        descriptor = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (descriptor < 0) goto done;
        sink.file = fdopen(descriptor, "wb");
        if (sink.file == NULL) goto done;
        descriptor = -1;
        sink.expected = job->size;
        sink.job = job;
        mbedtls_md_init(&sink.sha);
        if (mbedtls_md_setup(&sink.sha, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                             0) != 0 || mbedtls_md_starts(&sink.sha) != 0)
            goto cleanup;
        config.url = url;
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 30000;
        config.disable_auto_redirect = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = download_event;
        config.user_data = &sink;
        config.buffer_size = 1024;
        client = esp_http_client_init(&config);
        if (client != NULL) http_result = esp_http_client_perform(client);
        if (client == NULL || http_result != ESP_OK || sink.failed ||
            esp_http_client_get_status_code(client) != 200 ||
            sink.received != job->size ||
            mbedtls_md_finish(&sink.sha, digest) != 0 ||
            memcmp(digest, job->digest, sizeof(digest)) != 0 ||
            fflush(sink.file) != 0) {
            ESP_LOGW("PxaStore", "Download failed: http=%s code=%d bytes=%llu/%llu sink=%d",
                     esp_err_to_name(http_result),
                     client != NULL ? esp_http_client_get_status_code(client) : 0,
                     (unsigned long long)sink.received,
                     (unsigned long long)job->size, sink.failed);
            goto cleanup;
        }
        if (fclose(sink.file) != 0) { sink.file = NULL; goto cleanup; }
        sink.file = NULL;
    }
    if (!pxa_esp_package_store_preview_file(path, &job->preview) ||
        strcmp(job->preview.app_id, job->app_id) != 0) {
        ESP_LOGW("PxaStore", "Package preview rejected: app=%s", job->app_id);
        job->status = PXA_STATUS_DENIED;
        goto cleanup;
    }
    downloaded = 1;
    if (job->separate == 1u) {
        job->status = PXA_STATUS_OK;
        job->keep_file = 1;
        job->notify(job, 3u);
        goto cleanup;
    } else {
        job->notify(job, 1u);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    if (!job->approved) {
        job->status = PXA_STATUS_DENIED;
        goto cleanup;
    }
    job->status = pxa_host_install_package_file(path)
                      ? PXA_STATUS_OK : PXA_STATUS_INTERNAL;
    if (job->status != PXA_STATUS_OK) {
        size_t total = 0;
        size_t used = 0;
        struct stat source;
        if (stat(path, &source) == 0 && source.st_size > 0 &&
            esp_littlefs_info(CONFIG_PXA_STORAGE_PARTITION_LABEL,
                              &total, &used) == ESP_OK && used <= total &&
            total - used < (size_t)source.st_size * 2u)
            job->status = PXA_STATUS_QUOTA_EXCEEDED;
    }
cleanup:
    if (client != NULL) esp_http_client_cleanup(client);
    mbedtls_md_free(&sink.sha);
    if (sink.file != NULL) fclose(sink.file);
    if (!job->keep_file) unlink(path);
done:
    if (descriptor >= 0) { close(descriptor); unlink(path); }
    if (!downloaded && job->status == PXA_STATUS_IO_ERROR)
        job->status = PXA_STATUS_UNAVAILABLE;
    job->notify(job, 2u);
    vTaskDelete(NULL);
}
#endif
