#include "pxadb/pxadb_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>

#include "sdkconfig.h"

#if CONFIG_PXADB_ENABLED && CONFIG_IDF_TARGET_ESP32S3

#if CONFIG_PXADB_TRANSPORT_UART
#include <driver/gpio.h>
#include <driver/uart.h>
#else
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#endif
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_private/log_lock.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#if CONFIG_PXADB_TEST_CONTROL
#include "pxadb_input_mailbox.h"
#endif

#if CONFIG_PXA_ENABLED
#include "pxa/pxa_host.h"
#endif

namespace pxadb {
namespace {

constexpr char kTag[] = "Pxadb";
constexpr char kProtocol[] = "PXADB1";
constexpr size_t kMaxCommandLength = 2048;
constexpr size_t kMaxFramePayload = 320;
constexpr size_t kMaxLogFramePayload = kMaxFramePayload + 12;
constexpr size_t kMaxEncodedPayload = 448;
constexpr size_t kUsbBufferSize = 512;
constexpr size_t kLogQueueDepth = 4;
constexpr size_t kLogHistoryDepth = 16;
constexpr size_t kLogScratchCount = 2;
constexpr size_t kFsPathLength = 240;
#ifndef CONFIG_PXA_MOUNT_POINT
#define CONFIG_PXA_MOUNT_POINT "/assets"
#endif
#ifndef CONFIG_PXA_STATE_ROOT
#define CONFIG_PXA_STATE_ROOT "pxa-state"
#endif
constexpr char kPxaMountPoint[] = CONFIG_PXA_MOUNT_POINT;
constexpr char kPxaStateRoot[] = CONFIG_PXA_STATE_ROOT;
constexpr char kPxaInboxRoot[] = CONFIG_PXA_STATE_ROOT "/inbox";
constexpr size_t kFsAbsolutePathLength =
    kFsPathLength + sizeof(kPxaMountPoint) + 1;
constexpr size_t kFsDataChunkSize = 192;
// Host-to-device uploads are request/response paced, so a larger FSDATA chunk
// is the main throughput lever on UART links. Device-to-host FSGET frames
// stay at kFsDataChunkSize because they share the 320-byte frame payload.
constexpr size_t kFsUploadChunkSize = 1024;
#if CONFIG_PXA_ENABLED
constexpr size_t kPxaPackageListCapacity = 64;
#endif
// Package and file operations access LittleFS. ESP-IDF disables cache during
// Flash operations, so this task's stack must remain in internal SRAM.
constexpr uint32_t kTaskStackSize = 12 * 1024;
constexpr uint32_t kAutostartTaskStackSize = 3 * 1024;
constexpr uint32_t kSessionIdleTimeoutMs = 30000;
constexpr TickType_t kIoTimeout = pdMS_TO_TICKS(1000);
#if CONFIG_PXADB_TEST_CONTROL
constexpr uint32_t kInputTaskStackSize = 5 * 1024;
constexpr size_t kInputMailboxCapacity = 16;
constexpr uint16_t kDefaultTapDurationMs = 35;
constexpr uint16_t kInputTimeoutMs = 5000;
constexpr uint8_t kDefaultSwipeSteps = 12;
constexpr uint8_t kMaximumSwipeSteps = 120;
constexpr uint32_t kCaptureMinimumIntervalMs = 250;
#endif

#if CONFIG_PXADB_TRANSPORT_UART
constexpr uart_port_t kTransportPort =
    static_cast<uart_port_t>(CONFIG_PXADB_UART_PORT);
constexpr size_t kTransportRxBufferSize = 4096;
constexpr size_t kTransportTxBufferSize = 4096;
/* The console UART is installed with a 256-byte RX ring buffer, so poll it
 * often enough to drain a full command line without overflowing. */
constexpr uint32_t kTransportReadPollMs = 5;

esp_err_t TransportStart() {
    const uart_config_t config = {
        .baud_rate = CONFIG_PXADB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    if (uart_is_driver_installed(kTransportPort)) {
        /* Port 0 is normally owned by the console; adopt its driver and match
         * the configured baud rate instead of reinstalling it. */
        return uart_set_baudrate(kTransportPort, CONFIG_PXADB_UART_BAUD);
    }
    esp_err_t err = uart_param_config(kTransportPort, &config);
    if (err != ESP_OK) return err;
    const gpio_num_t tx_gpio = static_cast<gpio_num_t>(
        CONFIG_PXADB_UART_TX_GPIO >= 0
            ? CONFIG_PXADB_UART_TX_GPIO
            : static_cast<int>(UART_PIN_NO_CHANGE));
    const gpio_num_t rx_gpio = static_cast<gpio_num_t>(
        CONFIG_PXADB_UART_RX_GPIO >= 0
            ? CONFIG_PXADB_UART_RX_GPIO
            : static_cast<int>(UART_PIN_NO_CHANGE));
    if (tx_gpio != UART_PIN_NO_CHANGE || rx_gpio != UART_PIN_NO_CHANGE) {
        err = uart_set_pin(kTransportPort, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
        if (err != ESP_OK) return err;
    }
    return uart_driver_install(kTransportPort, kTransportRxBufferSize,
                               kTransportTxBufferSize, 0, nullptr, 0);
}

int TransportWrite(const uint8_t* data, size_t length) {
    return uart_write_bytes(kTransportPort, data, length);
}

bool TransportTxDone() {
    return uart_wait_tx_done(kTransportPort, kIoTimeout) == ESP_OK;
}

int TransportRead(uint8_t* buffer, size_t length, uint32_t timeout_ms) {
    return uart_read_bytes(kTransportPort, buffer, length,
                           pdMS_TO_TICKS(timeout_ms));
}
#else
constexpr uint32_t kTransportReadPollMs = 25;

esp_err_t TransportStart() {
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = kUsbBufferSize,
            .rx_buffer_size = kUsbBufferSize,
        };
        const esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) return err;
    }
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
}

int TransportWrite(const uint8_t* data, size_t length) {
    return usb_serial_jtag_write_bytes(data, length, kIoTimeout);
}

bool TransportTxDone() {
    return usb_serial_jtag_wait_tx_done(kIoTimeout) == ESP_OK;
}

int TransportRead(uint8_t* buffer, size_t length, uint32_t timeout_ms) {
    return usb_serial_jtag_read_bytes(buffer, length, pdMS_TO_TICKS(timeout_ms));
}
#endif

struct LogRecord {
    uint32_t timestamp_ms;
    char message[kMaxFramePayload];
};

struct LogScratch {
    LogRecord record = {};
    bool claimed = false;
};

struct FileUpload {
    FILE* file = nullptr;
    size_t total = 0;
    size_t remaining = 0;
    char destination[kFsAbsolutePathLength] = {};
    char temporary[kFsAbsolutePathLength + 16] = {};
    char expected_sha256[65] = {};
    bool has_expected_sha256 = false;
    bool refresh_pxa_inbox = false;
};

std::atomic<bool> s_running{false};
std::atomic<bool> s_task_active{false};
std::atomic<bool> s_log_subscribed{false};
std::atomic<uint32_t> s_dropped_logs{0};
std::atomic<bool> s_autostart_active{false};
std::atomic<bool> s_autostart_cancelled{false};
std::atomic<bool> s_control_session_active{false};
std::atomic<uint64_t> s_last_control_activity_us{0};
QueueHandle_t s_log_queue = nullptr;
SemaphoreHandle_t s_tx_mutex = nullptr;
StaticSemaphore_t s_tx_mutex_storage = {};
vprintf_like_t s_previous_vprintf = nullptr;
portMUX_TYPE s_history_lock = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE s_log_scratch_lock = portMUX_INITIALIZER_UNLOCKED;
LogRecord* s_log_history = nullptr;
size_t s_log_history_start = 0;
size_t s_log_history_count = 0;
LogScratch s_log_scratch[kLogScratchCount];
FileUpload s_file_upload;

const char* DeviceSerial() {
    static char serial[18] = {};
    static bool initialized = false;
    if (!initialized) {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(serial, sizeof(serial), "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            snprintf(serial, sizeof(serial), "unavailable");
        }
        initialized = true;
    }
    return serial;
}

#if CONFIG_PXADB_TEST_CONTROL
InputMailbox<kInputMailboxCapacity> s_input_mailbox;
PointerState s_accepted_pointer;
portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t s_input_signal = nullptr;
StaticSemaphore_t s_input_signal_storage = {};
std::atomic<bool> s_input_task_active{false};
std::atomic<uint32_t> s_next_input_event{1};
uint64_t s_last_capture_us = 0;
TestControlAdapter s_test_control;
#endif

#if CONFIG_PXADB_TEST_CONTROL
bool TestControlAvailable() {
    return s_test_control.struct_size >= sizeof(TestControlAdapter) &&
           s_test_control.width != 0 && s_test_control.height != 0 &&
           s_test_control.route_pointer_down != nullptr &&
           s_test_control.inject_pointer != nullptr &&
           s_test_control.cancel_pointer != nullptr &&
           s_test_control.route_key != nullptr &&
           s_test_control.capture_rgb565 != nullptr;
}
#endif

bool WriteAll(const void* data, size_t length) {
    if (s_tx_mutex != nullptr && xSemaphoreTake(s_tx_mutex, kIoTimeout) != pdTRUE) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    bool success = true;
    while (offset < length) {
        const int written = TransportWrite(bytes + offset, length - offset);
        if (written <= 0) {
            success = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (success) success = TransportTxDone();
    if (s_tx_mutex != nullptr) xSemaphoreGive(s_tx_mutex);
    return success;
}

bool SendFrame(unsigned long sequence, const char* type, const char* payload) {
    const size_t payload_length = payload == nullptr ? 0 : strlen(payload);
    char encoded[kMaxEncodedPayload] = {};
    size_t encoded_length = 0;
    if (payload_length > 0 &&
        mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded), sizeof(encoded) - 1,
                              &encoded_length, reinterpret_cast<const unsigned char*>(payload),
                              payload_length) != 0) {
        return false;
    }
    encoded[encoded_length] = '\0';

    char line[kMaxEncodedPayload + 64] = {};
    const int length = snprintf(line, sizeof(line), "\n%s %lu %s %s\n", kProtocol, sequence, type,
                                payload_length == 0 ? "-" : encoded);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) return false;
    // ESP_LOG uses the same USB Serial/JTAG output. Keep raw console text on
    // either side of a complete response so the host can always resynchronize.
    esp_log_impl_lock();
    const bool sent = WriteAll(line, static_cast<size_t>(length));
    esp_log_impl_unlock();
    return sent;
}

bool IsSafeIdentity(const char* identity) {
    if (identity == nullptr || identity[0] == '\0' || strlen(identity) > 64 ||
        identity[0] < 'a' || identity[0] > 'z') {
        return false;
    }
    for (const char* character = identity; *character != '\0'; ++character) {
        const bool allowed = (*character >= 'a' && *character <= 'z') ||
                             (*character >= '0' && *character <= '9') ||
                             *character == '.' || *character == '_' || *character == '-';
        if (!allowed) return false;
    }
    return true;
}

void AbortFileUpload() {
    if (s_file_upload.file != nullptr) {
        fclose(s_file_upload.file);
        s_file_upload.file = nullptr;
    }
    if (s_file_upload.temporary[0] != '\0') unlink(s_file_upload.temporary);
    s_file_upload = {};
}

bool DecodeFsPath(const char* encoded, char* relative, size_t relative_capacity,
                  char* absolute, size_t absolute_capacity) {
    if (encoded == nullptr || relative == nullptr || absolute == nullptr ||
        relative_capacity < 2 || absolute_capacity < sizeof(kPxaMountPoint)) {
        return false;
    }
    size_t decoded_length = 0;
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(relative),
                              relative_capacity - 1, &decoded_length,
                              reinterpret_cast<const unsigned char*>(encoded),
                              strlen(encoded)) != 0 ||
        decoded_length == 0 || decoded_length >= relative_capacity) {
        return false;
    }
    relative[decoded_length] = '\0';
    if (strcmp(relative, ".") == 0) {
        const int length = snprintf(absolute, absolute_capacity, "%s", kPxaMountPoint);
        return length > 0 && static_cast<size_t>(length) < absolute_capacity;
    }
    if (relative[0] == '/') return false;
    const char* segment = relative;
    for (char* character = relative;; ++character) {
        const char value = *character;
        if (!(value == '\0' || value == '/' || value == '.' || value == '_' ||
              value == '-' || (value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))) {
            return false;
        }
        if (value == '/' || value == '\0') {
            if (character == segment ||
                (character - segment == 1 && segment[0] == '.') ||
                (character - segment == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (value == '\0') break;
            segment = character + 1;
        }
    }
    const int length = snprintf(absolute, absolute_capacity, "%s/%s", kPxaMountPoint,
                                relative);
    return length > 0 && static_cast<size_t>(length) < absolute_capacity;
}

bool IsWritableFsPath(const char* relative) {
    if (relative == nullptr) return false;
    return strcmp(relative, kPxaStateRoot) == 0 ||
           (strncmp(relative, kPxaInboxRoot, sizeof(kPxaInboxRoot) - 1) == 0 &&
            (relative[sizeof(kPxaInboxRoot) - 1] == '\0' ||
             relative[sizeof(kPxaInboxRoot) - 1] == '/'));
}

bool IsPxaInboxPath(const char* relative) {
    return relative != nullptr &&
           strncmp(relative, kPxaInboxRoot, sizeof(kPxaInboxRoot) - 1) == 0 &&
           (relative[sizeof(kPxaInboxRoot) - 1] == '\0' ||
            relative[sizeof(kPxaInboxRoot) - 1] == '/');
}

bool ShouldRefreshPxaInboxAfterUpload(const char* relative) {
    if (!IsPxaInboxPath(relative)) return false;
    const char* filename = strrchr(relative, '/');
    filename = filename == nullptr ? relative : filename + 1;
    const size_t length = strlen(filename);
    return strcmp(filename, "manifest.pxm") == 0 ||
           (length > 4 && strcmp(filename + length - 4, ".pxa") == 0);
}

void RefreshPxaInboxAfterFsMutation() {
#if CONFIG_PXA_ENABLED
    if (pxa_host_ready() && !pxa_host_refresh_inbox()) {
        ESP_LOGW(kTag, "PXA Inbox refresh failed after filesystem mutation");
    }
#endif
}

bool DecodeFileChunk(const char* encoded, uint8_t* data, size_t capacity,
                     size_t* data_length) {
    if (encoded == nullptr || data == nullptr || data_length == nullptr) return false;
    return mbedtls_base64_decode(data, capacity, data_length,
                                 reinterpret_cast<const unsigned char*>(encoded),
                                 strlen(encoded)) == 0;
}

bool IsSha256Hex(const char* value) {
    if (value == nullptr || strlen(value) != 64) return false;
    for (const char* character = value; *character != '\0'; ++character) {
        if (!((*character >= '0' && *character <= '9') ||
              (*character >= 'a' && *character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool FileMatchesSha256(const char* path, const char* expected) {
    if (path == nullptr || !IsSha256Hex(expected)) return false;
    FILE* file = fopen(path, "rb");
    if (file == nullptr) return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool success = mbedtls_sha256_starts(&context, 0) == 0;
    uint8_t bytes[512] = {};
    while (success) {
        const size_t read = fread(bytes, 1, sizeof(bytes), file);
        if (read > 0) success = mbedtls_sha256_update(&context, bytes, read) == 0;
        if (read < sizeof(bytes)) {
            success = success && ferror(file) == 0;
            break;
        }
    }
    uint8_t digest[32] = {};
    success = success && mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    fclose(file);
    if (!success) return false;
    char actual[65] = {};
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(actual + index * 2, sizeof(actual) - index * 2, "%02x", digest[index]);
    }
    return strcmp(actual, expected) == 0;
}

void SendFsUploadReady(unsigned long sequence) {
    char payload[32] = {};
    snprintf(payload, sizeof(payload), "%u", static_cast<unsigned>(s_file_upload.remaining));
    SendFrame(sequence, "READY", payload);
}

void SendFsList(unsigned long sequence, const char* encoded_path) {
    char relative[kFsPathLength] = {};
    char path[kFsAbsolutePathLength] = {};
    if (!DecodeFsPath(encoded_path, relative, sizeof(relative), path, sizeof(path))) {
        SendFrame(sequence, "ERR", "invalid_path");
        return;
    }
    DIR* directory = opendir(path);
    if (directory == nullptr) {
        SendFrame(sequence, "ERR", "path_not_found");
        return;
    }
    bool sent = true;
    while (sent) {
        struct dirent* entry = readdir(directory);
        if (entry == nullptr) break;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[kFsAbsolutePathLength] = {};
        const int child_length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (child_length < 0 || static_cast<size_t>(child_length) >= sizeof(child)) continue;
        struct stat metadata = {};
        if (stat(child, &metadata) != 0) continue;
        char payload[kMaxFramePayload] = {};
        const char kind = S_ISDIR(metadata.st_mode) ? 'D' : 'F';
        const uint32_t size = metadata.st_size > UINT32_MAX
                                  ? UINT32_MAX
                                  : static_cast<uint32_t>(metadata.st_size);
        const int payload_length = snprintf(payload, sizeof(payload), "%c\t%u\t%s", kind,
                                            static_cast<unsigned>(size), entry->d_name);
        if (payload_length < 0 || static_cast<size_t>(payload_length) >= sizeof(payload) ||
            !SendFrame(sequence, "ENTRY", payload)) {
            sent = false;
        }
    }
    closedir(directory);
    if (sent) SendFrame(sequence, "OK", "");
}

void SendFsFile(unsigned long sequence, const char* encoded_path) {
    char relative[kFsPathLength] = {};
    char path[kFsAbsolutePathLength] = {};
    if (!DecodeFsPath(encoded_path, relative, sizeof(relative), path, sizeof(path))) {
        SendFrame(sequence, "ERR", "invalid_path");
        return;
    }
    struct stat metadata = {};
    if (stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        SendFrame(sequence, "ERR", "file_not_found");
        return;
    }
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        SendFrame(sequence, "ERR", "file_open_failed");
        return;
    }
    bool sent = true;
    while (sent) {
        uint8_t bytes[kFsDataChunkSize] = {};
        const size_t read = fread(bytes, 1, sizeof(bytes), file);
        if (read == 0) break;
        char payload[kFsDataChunkSize * 2] = {};
        size_t payload_length = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(payload),
                                  sizeof(payload) - 1, &payload_length, bytes,
                                  read) != 0) {
            sent = false;
            break;
        }
        payload[payload_length] = '\0';
        sent = SendFrame(sequence, "DATA", payload);
    }
    const bool read_failed = ferror(file) != 0;
    fclose(file);
    if (!sent) return;
    SendFrame(sequence, read_failed ? "ERR" : "OK",
              read_failed ? "file_read_failed" : "");
}

void StartFsUpload(unsigned long sequence, const char* encoded_path,
                   const char* size_text, const char* expected_sha256) {
    char relative[kFsPathLength] = {};
    char path[kFsAbsolutePathLength] = {};
    char* end = nullptr;
    const unsigned long size = size_text == nullptr ? 0 : strtoul(size_text, &end, 10);
    if (size_text == nullptr || end == size_text || *end != '\0' ||
        !DecodeFsPath(encoded_path, relative, sizeof(relative), path, sizeof(path)) ||
        !IsWritableFsPath(relative)) {
        SendFrame(sequence, "ERR", "invalid_path_or_size");
        return;
    }
    const bool has_expected_sha256 = expected_sha256 != nullptr;
    if (has_expected_sha256 && !IsSha256Hex(expected_sha256)) {
        SendFrame(sequence, "ERR", "invalid_sha256");
        return;
    }
    if (s_file_upload.file != nullptr && s_file_upload.total == static_cast<size_t>(size) &&
        strcmp(s_file_upload.destination, path) == 0 &&
        s_file_upload.has_expected_sha256 == has_expected_sha256 &&
        (!has_expected_sha256 || strcmp(s_file_upload.expected_sha256, expected_sha256) == 0)) {
        SendFsUploadReady(sequence);
        return;
    }
    struct stat existing = {};
    if (has_expected_sha256 && stat(path, &existing) == 0 && S_ISREG(existing.st_mode) &&
        existing.st_size == static_cast<off_t>(size) && FileMatchesSha256(path, expected_sha256)) {
        SendFrame(sequence, "OK", "");
        return;
    }
    AbortFileUpload();
    const int temporary_length = snprintf(s_file_upload.temporary,
                                          sizeof(s_file_upload.temporary), "%s.pxadb-part", path);
    if (temporary_length < 0 ||
        static_cast<size_t>(temporary_length) >= sizeof(s_file_upload.temporary)) {
        AbortFileUpload();
        SendFrame(sequence, "ERR", "path_too_long");
        return;
    }
    s_file_upload.file = fopen(s_file_upload.temporary, "wb");
    if (s_file_upload.file == nullptr) {
        AbortFileUpload();
        SendFrame(sequence, "ERR", "file_open_failed");
        return;
    }
    snprintf(s_file_upload.destination, sizeof(s_file_upload.destination), "%s", path);
    s_file_upload.total = static_cast<size_t>(size);
    s_file_upload.remaining = static_cast<size_t>(size);
    s_file_upload.has_expected_sha256 = has_expected_sha256;
    if (has_expected_sha256) {
        snprintf(s_file_upload.expected_sha256, sizeof(s_file_upload.expected_sha256), "%s", expected_sha256);
    }
    s_file_upload.refresh_pxa_inbox =
        ShouldRefreshPxaInboxAfterUpload(relative);
    if (s_file_upload.remaining == 0) {
        const bool refresh_pxa_inbox = s_file_upload.refresh_pxa_inbox;
        fclose(s_file_upload.file);
        s_file_upload.file = nullptr;
        const bool checksum_matches = !s_file_upload.has_expected_sha256 ||
                                      FileMatchesSha256(s_file_upload.temporary,
                                                        s_file_upload.expected_sha256);
        if (!checksum_matches) {
            AbortFileUpload();
            SendFrame(sequence, "ERR", "checksum_mismatch");
            return;
        }
        const bool renamed = rename(s_file_upload.temporary, s_file_upload.destination) == 0;
        if (!renamed) AbortFileUpload();
        else {
            s_file_upload = {};
            if (refresh_pxa_inbox) RefreshPxaInboxAfterFsMutation();
        }
        SendFrame(sequence, renamed ? "OK" : "ERR",
                  renamed ? "" : "file_commit_failed");
        return;
    }
    SendFsUploadReady(sequence);
}

void WriteFsUpload(unsigned long sequence, const char* offset_text, const char* encoded_data) {
    if (s_file_upload.file == nullptr) {
        SendFrame(sequence, "ERR", "no_upload_in_progress");
        return;
    }
    const bool has_offset = encoded_data != nullptr;
    if (!has_offset) encoded_data = offset_text;
    const size_t expected_offset = s_file_upload.total - s_file_upload.remaining;
    if (has_offset) {
        if (offset_text == nullptr) {
            SendFrame(sequence, "ERR", "offset_mismatch");
            return;
        }
        char* end = nullptr;
        const unsigned long parsed_offset = strtoul(offset_text, &end, 10);
        if (end == offset_text || *end != '\0' || parsed_offset > expected_offset) {
            SendFrame(sequence, "ERR", "offset_mismatch");
            return;
        }
        if (static_cast<size_t>(parsed_offset) < expected_offset) {
            SendFsUploadReady(sequence);
            return;
        }
    }
    uint8_t bytes[kFsUploadChunkSize] = {};
    size_t length = 0;
    if (!DecodeFileChunk(encoded_data, bytes, sizeof(bytes), &length) || length == 0 ||
        length > s_file_upload.remaining ||
        fwrite(bytes, 1, length, s_file_upload.file) != length) {
        AbortFileUpload();
        SendFrame(sequence, "ERR", "file_write_failed");
        return;
    }
    s_file_upload.remaining -= length;
    if (s_file_upload.remaining != 0) {
        SendFsUploadReady(sequence);
        return;
    }
    const bool refresh_pxa_inbox = s_file_upload.refresh_pxa_inbox;
    const bool has_expected_sha256 = s_file_upload.has_expected_sha256;
    char expected_sha256[sizeof(s_file_upload.expected_sha256)] = {};
    snprintf(expected_sha256, sizeof(expected_sha256), "%s", s_file_upload.expected_sha256);
    char temporary[sizeof(s_file_upload.temporary)] = {};
    snprintf(temporary, sizeof(temporary), "%s", s_file_upload.temporary);
    fclose(s_file_upload.file);
    s_file_upload.file = nullptr;
    if (has_expected_sha256 && !FileMatchesSha256(temporary, expected_sha256)) {
        AbortFileUpload();
        SendFrame(sequence, "ERR", "checksum_mismatch");
        return;
    }
    const bool renamed = rename(s_file_upload.temporary, s_file_upload.destination) == 0;
    if (!renamed) {
        AbortFileUpload();
        SendFrame(sequence, "ERR", "file_commit_failed");
        return;
    }
    s_file_upload = {};
    if (refresh_pxa_inbox) RefreshPxaInboxAfterFsMutation();
    SendFrame(sequence, "OK", "");
}

void MakeFsDirectory(unsigned long sequence, const char* encoded_path) {
    char relative[kFsPathLength] = {};
    char path[kFsAbsolutePathLength] = {};
    if (!DecodeFsPath(encoded_path, relative, sizeof(relative), path, sizeof(path)) ||
        !IsWritableFsPath(relative)) {
        SendFrame(sequence, "ERR", "invalid_path");
        return;
    }
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        SendFrame(sequence, "OK", "");
    } else {
        SendFrame(sequence, "ERR", "mkdir_failed");
    }
}

void RemoveFsPath(unsigned long sequence, const char* encoded_path) {
    char relative[kFsPathLength] = {};
    char path[kFsAbsolutePathLength] = {};
    if (!DecodeFsPath(encoded_path, relative, sizeof(relative), path, sizeof(path)) ||
        !IsWritableFsPath(relative) || strcmp(relative, "pxa-state") == 0) {
        SendFrame(sequence, "ERR", "invalid_path");
        return;
    }
    if (unlink(path) == 0 || rmdir(path) == 0) {
        if (IsPxaInboxPath(relative)) RefreshPxaInboxAfterFsMutation();
        SendFrame(sequence, "OK", "");
    } else {
        SendFrame(sequence, "ERR", errno == ENOTEMPTY ? "directory_not_empty" : "remove_failed");
    }
}

void RememberLog(const LogRecord& record) {
    if (s_log_history == nullptr) return;
    portENTER_CRITICAL(&s_history_lock);
    const size_t destination = (s_log_history_start + s_log_history_count) % kLogHistoryDepth;
    s_log_history[destination] = record;
    if (s_log_history_count < kLogHistoryDepth) {
        ++s_log_history_count;
    } else {
        s_log_history_start = (s_log_history_start + 1) % kLogHistoryDepth;
    }
    portEXIT_CRITICAL(&s_history_lock);
}

LogScratch* AcquireLogScratch() {
    LogScratch* scratch = nullptr;
    portENTER_CRITICAL(&s_log_scratch_lock);
    for (size_t index = 0; index < kLogScratchCount; ++index) {
        if (!s_log_scratch[index].claimed) {
            s_log_scratch[index].claimed = true;
            scratch = &s_log_scratch[index];
            break;
        }
    }
    portEXIT_CRITICAL(&s_log_scratch_lock);
    return scratch;
}

void ReleaseLogScratch(LogScratch* scratch) {
    if (scratch == nullptr) return;
    portENTER_CRITICAL(&s_log_scratch_lock);
    scratch->claimed = false;
    portEXIT_CRITICAL(&s_log_scratch_lock);
}

int PxaLogVprintf(const char* format, va_list arguments);

void EnableLogCapture() {
    // Keep system tasks on ESP-IDF's original log path until a PXADB client asks
    // for logcat; some of those tasks intentionally have small stacks.
    if (s_previous_vprintf == nullptr)
        s_previous_vprintf = esp_log_set_vprintf(PxaLogVprintf);
    s_log_subscribed.store(true);
}

void DisableLogCapture() {
    s_log_subscribed.store(false);
    if (s_previous_vprintf != nullptr) {
        vprintf_like_t previous = s_previous_vprintf;
        s_previous_vprintf = nullptr;
        esp_log_set_vprintf(previous);
    }
}

int PxaLogVprintf(const char* format, va_list arguments) {
    LogScratch* scratch = AcquireLogScratch();
    if (scratch == nullptr) {
        if (s_log_subscribed.load()) s_dropped_logs.fetch_add(1);
        if (s_previous_vprintf != nullptr) {
            va_list forwarded;
            va_copy(forwarded, arguments);
            const int result = s_previous_vprintf(format, forwarded);
            va_end(forwarded);
            return result;
        }
        return 0;
    }
    LogRecord& record = scratch->record;
    record.timestamp_ms = static_cast<uint32_t>(esp_log_timestamp());
    va_list copied;
    va_copy(copied, arguments);
    const int result = vsnprintf(record.message, sizeof(record.message), format, copied);
    va_end(copied);
    size_t length = strnlen(record.message, sizeof(record.message));
    while (length > 0 && (record.message[length - 1] == '\n' || record.message[length - 1] == '\r')) {
        record.message[--length] = '\0';
    }
    RememberLog(record);
    if (s_log_subscribed.load() && s_log_queue != nullptr &&
        xQueueSend(s_log_queue, &record, 0) != pdTRUE) {
        s_dropped_logs.fetch_add(1);
    }
    ReleaseLogScratch(scratch);
    if (!s_log_subscribed.load() && s_previous_vprintf != nullptr) {
        va_list forwarded;
        va_copy(forwarded, arguments);
        const int forwarded_result = s_previous_vprintf(format, forwarded);
        va_end(forwarded);
        return forwarded_result;
    }
    return result;
}

void SendHello(unsigned long sequence) {
    const esp_app_desc_t* app = esp_app_get_description();
    char payload[kMaxFramePayload] = {};
    const char* kTestCapabilities =
#if CONFIG_PXADB_TEST_CONTROL
        TestControlAvailable() ? ",input-v2,screenshot-rgb565" : "";
#else
        "";
#endif
    snprintf(payload, sizeof(payload),
             "protocol=1;mode=normal;capabilities=info,logcat,packages,package-deploy,fs,reboot,doctor%s;max_chunk=%u;fs_offset=1;fs_sha256=1;target=%s;serial=%s;version=%s",
             kTestCapabilities,
             static_cast<unsigned>(kFsUploadChunkSize),
             CONFIG_IDF_TARGET, DeviceSerial(), app == nullptr ? "unknown" : app->version);
    SendFrame(sequence, "OK", payload);
}

void SendInfo(unsigned long sequence) {
    const esp_app_desc_t* app = esp_app_get_description();
    char payload[kMaxFramePayload] = {};
    snprintf(payload, sizeof(payload), "target=%s;serial=%s;version=%s;free_heap=%u;pxa=%s",
             CONFIG_IDF_TARGET, DeviceSerial(),
             app == nullptr ? "unknown" : app->version,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
#if CONFIG_PXA_ENABLED
             pxa_host_ready() ? "ready" : "unavailable"
#else
             "disabled"
#endif
    );
    SendFrame(sequence, "OK", payload);
}

#if CONFIG_PXADB_TEST_CONTROL
bool ParseUnsigned(const char* text, uint32_t maximum, uint32_t* value) {
    if (text == nullptr || value == nullptr || text[0] == '-') return false;
    char* end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > maximum)
        return false;
    *value = static_cast<uint32_t>(parsed);
    return true;
}

void SendInputAck(const InputCommand& command, const char* status) {
    if (command.request_sequence == 0) return;
    char payload[112] = {};
    snprintf(payload, sizeof(payload),
             "event=%u;device_us=%" PRIu64 ";status=%s",
             static_cast<unsigned>(command.event_sequence),
             static_cast<uint64_t>(esp_timer_get_time()), status);
    SendFrame(command.request_sequence, "OK", payload);
}

bool QueueInput(const InputCommand& requested, const char** error) {
    InputCommand command = requested;
    InputCommand replaced = {};
    PointerState next_state;
    MailboxPushResult result = MailboxPushResult::kFull;
    bool valid = true;
    portENTER_CRITICAL(&s_input_lock);
    next_state = s_accepted_pointer;
    if (command.kind == InputCommandKind::kPointer) {
        valid = next_state.Apply(command.pointer_action, command.pointer_id);
    } else if (command.kind == InputCommandKind::kTap ||
               command.kind == InputCommandKind::kSwipe) {
        valid = !next_state.pressed();
    } else if (command.kind == InputCommandKind::kCancel) {
        next_state.Reset();
    } else if (command.kind == InputCommandKind::kKey) {
        next_state.Reset();
    }
    if (valid) {
        result = s_input_mailbox.Push(command, &replaced);
        if (result != MailboxPushResult::kFull) {
            s_accepted_pointer = next_state;
        }
    }
    portEXIT_CRITICAL(&s_input_lock);
    if (!valid) {
        *error = "invalid_pointer_state";
        return false;
    }
    if (result == MailboxPushResult::kFull) {
        *error = "input_queue_full";
        return false;
    }
    if (replaced.event_sequence != 0) SendInputAck(replaced, "replaced");
    xSemaphoreGive(s_input_signal);
    return true;
}

void QueueSessionCancel() {
    InputCommand command;
    command.kind = InputCommandKind::kCancel;
    command.event_sequence = s_next_input_event.fetch_add(1);
    const char* error = nullptr;
    (void)QueueInput(command, &error);
}

void HandleSystemInputTakeover(void*) {
    if (s_running.load()) QueueSessionCancel();
}

bool ParsePoint(const char* x_text, const char* y_text, uint16_t width,
                uint16_t height, uint16_t* x, uint16_t* y) {
    uint32_t parsed_x = 0;
    uint32_t parsed_y = 0;
    if (!ParseUnsigned(x_text, UINT16_MAX, &parsed_x) ||
        !ParseUnsigned(y_text, UINT16_MAX, &parsed_y) ||
        !CoordinateInBounds(static_cast<uint16_t>(parsed_x),
                            static_cast<uint16_t>(parsed_y), width, height)) {
        return false;
    }
    *x = static_cast<uint16_t>(parsed_x);
    *y = static_cast<uint16_t>(parsed_y);
    return true;
}

void SendInputCapabilities(unsigned long sequence) {
    if (!TestControlAvailable()) {
        SendFrame(sequence, "ERR", "input_unavailable");
        return;
    }
    char payload[160] = {};
    snprintf(payload, sizeof(payload),
             "protocol=1;width=%d;height=%d;max_pointers=1;keys=back,home,volume-up,volume-down;coordinates=logical",
             static_cast<int>(s_test_control.width),
             static_cast<int>(s_test_control.height));
    SendFrame(sequence, "OK", payload);
}

void HandleInput(unsigned long sequence, char* const* arguments,
                 size_t argument_count) {
    if (argument_count == 0 || arguments[0] == nullptr) {
        SendFrame(sequence, "ERR", "invalid_input");
        return;
    }
    if (strcmp(arguments[0], "CAPABILITIES") == 0) {
        SendInputCapabilities(sequence);
        return;
    }
    if (!TestControlAvailable()) {
        SendFrame(sequence, "ERR", "input_unavailable");
        return;
    }
    const uint16_t width = s_test_control.width;
    const uint16_t height = s_test_control.height;
    InputCommand command;
    command.request_sequence = static_cast<uint32_t>(sequence);
    command.event_sequence = s_next_input_event.fetch_add(1);

    const bool pointer = strcmp(arguments[0], "POINTER") == 0 ||
                         strcmp(arguments[0], "TOUCH") == 0;
    if (pointer) {
        if (argument_count < 4) {
            SendFrame(sequence, "ERR", "invalid_pointer");
            return;
        }
        command.kind = InputCommandKind::kPointer;
        if (strcmp(arguments[1], "DOWN") == 0) {
            command.pointer_action = PointerAction::kDown;
        } else if (strcmp(arguments[1], "MOVE") == 0) {
            command.pointer_action = PointerAction::kMove;
        } else if (strcmp(arguments[1], "UP") == 0) {
            command.pointer_action = PointerAction::kUp;
        } else if (strcmp(arguments[1], "CANCEL") == 0) {
            command.pointer_action = PointerAction::kCancel;
        } else {
            SendFrame(sequence, "ERR", "invalid_pointer_action");
            return;
        }
        if (!ParsePoint(arguments[2], arguments[3], width, height,
                        &command.x, &command.y)) {
            SendFrame(sequence, "ERR", "pointer_out_of_bounds");
            return;
        }
        if (argument_count >= 5) {
            uint32_t id = 0;
            if (!ParseUnsigned(arguments[4], UINT8_MAX, &id)) {
                SendFrame(sequence, "ERR", "invalid_pointer_id");
                return;
            }
            command.pointer_id = static_cast<uint8_t>(id);
        }
    } else if (strcmp(arguments[0], "TAP") == 0) {
        command.kind = InputCommandKind::kTap;
        command.duration_ms = kDefaultTapDurationMs;
        if (argument_count != 3 ||
            !ParsePoint(arguments[1], arguments[2], width, height,
                        &command.x, &command.y)) {
            SendFrame(sequence, "ERR", "invalid_tap");
            return;
        }
    } else if (strcmp(arguments[0], "SWIPE") == 0) {
        command.kind = InputCommandKind::kSwipe;
        uint32_t duration = 0;
        uint32_t steps = kDefaultSwipeSteps;
        if ((argument_count != 6 && argument_count != 7) ||
            !ParsePoint(arguments[1], arguments[2], width, height,
                        &command.x, &command.y) ||
            !ParsePoint(arguments[3], arguments[4], width, height,
                        &command.end_x, &command.end_y) ||
            !ParseUnsigned(arguments[5], UINT16_MAX, &duration) ||
            duration == 0 ||
            (argument_count == 7 &&
             !ParseUnsigned(arguments[6], kMaximumSwipeSteps, &steps)) ||
            steps == 0) {
            SendFrame(sequence, "ERR", "invalid_swipe");
            return;
        }
        command.duration_ms = static_cast<uint16_t>(duration);
        command.steps = static_cast<uint8_t>(steps);
    } else if (strcmp(arguments[0], "KEY") == 0) {
        command.kind = InputCommandKind::kKey;
        if (argument_count != 2) {
            SendFrame(sequence, "ERR", "invalid_key");
            return;
        }
        if (strcmp(arguments[1], "BACK") == 0) {
            command.key = InputKey::kBack;
        } else if (strcmp(arguments[1], "HOME") == 0) {
            command.key = InputKey::kHome;
        } else if (strcmp(arguments[1], "VOLUME-UP") == 0) {
            command.key = InputKey::kVolumeUp;
        } else if (strcmp(arguments[1], "VOLUME-DOWN") == 0) {
            command.key = InputKey::kVolumeDown;
        } else {
            SendFrame(sequence, "ERR", "invalid_key");
            return;
        }
    } else if (strcmp(arguments[0], "CANCEL") == 0) {
        command.kind = InputCommandKind::kCancel;
    } else if (strcmp(arguments[0], "SYNC") == 0) {
        command.kind = InputCommandKind::kSync;
    } else {
        SendFrame(sequence, "ERR", "invalid_input_kind");
        return;
    }

    const char* error = nullptr;
    if (!QueueInput(command, &error)) SendFrame(sequence, "ERR", error);
}

bool Sha256Bytes(const uint8_t* bytes, size_t length, char output[65]) {
    uint8_t digest[32] = {};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    const bool success = mbedtls_sha256_starts(&context, 0) == 0 &&
                         mbedtls_sha256_update(&context, bytes, length) == 0 &&
                         mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!success) return false;
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(output + index * 2, 65 - index * 2, "%02x", digest[index]);
    }
    return true;
}

void SendScreenshot(unsigned long sequence, bool after_present) {
    const uint64_t requested_us = static_cast<uint64_t>(esp_timer_get_time());
    if (s_last_capture_us != 0 &&
        requested_us - s_last_capture_us <
            static_cast<uint64_t>(kCaptureMinimumIntervalMs) * 1000u) {
        SendFrame(sequence, "ERR", "screenshot_rate_limited");
        return;
    }
    s_last_capture_us = requested_us;
    if (!TestControlAvailable()) {
        SendFrame(sequence, "ERR", "screenshot_unavailable");
        return;
    }
    const size_t pixel_count = static_cast<size_t>(s_test_control.width) *
                               static_cast<size_t>(s_test_control.height);
    const size_t byte_count = pixel_count * sizeof(uint16_t);
    auto* pixels = static_cast<uint16_t*>(heap_caps_malloc(
        byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    TestControlCaptureInfo info;
    if (pixels == nullptr ||
        !s_test_control.capture_rgb565(s_test_control.context, pixels,
                                      pixel_count, after_present, &info)) {
        if (pixels != nullptr) heap_caps_free(pixels);
        SendFrame(sequence, "ERR", "screenshot_failed");
        return;
    }
    if (info.width != static_cast<uint32_t>(s_test_control.width) ||
        info.height != static_cast<uint32_t>(s_test_control.height) ||
        info.stride_bytes != info.width * sizeof(uint16_t)) {
        heap_caps_free(pixels);
        SendFrame(sequence, "ERR", "screenshot_layout_changed");
        return;
    }
    char sha256[65] = {};
    if (!Sha256Bytes(reinterpret_cast<const uint8_t*>(pixels), byte_count,
                     sha256)) {
        heap_caps_free(pixels);
        SendFrame(sequence, "ERR", "screenshot_checksum_failed");
        return;
    }
    char metadata[kMaxFramePayload] = {};
    snprintf(metadata, sizeof(metadata),
             "format=rgb565le;width=%u;height=%u;stride=%u;orientation=logical;frame_id=%" PRIu64 ";device_us=%" PRIu64 ";source=%s;bytes=%u;sha256=%s",
             static_cast<unsigned>(info.width),
             static_cast<unsigned>(info.height),
             static_cast<unsigned>(info.stride_bytes), info.frame_id,
             info.completed_timestamp_us, info.source,
             static_cast<unsigned>(byte_count), sha256);
    if (!SendFrame(sequence, "META", metadata)) {
        heap_caps_free(pixels);
        return;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(pixels);
    for (size_t offset = 0; offset < byte_count;
         offset += kFsDataChunkSize) {
        const size_t count = std::min(kFsDataChunkSize, byte_count - offset);
        char encoded[kFsDataChunkSize * 2] = {};
        size_t encoded_length = 0;
        if (mbedtls_base64_encode(
                reinterpret_cast<unsigned char*>(encoded), sizeof(encoded) - 1,
                &encoded_length,
                bytes + offset, count) != 0) {
            heap_caps_free(pixels);
            SendFrame(sequence, "ERR", "screenshot_encode_failed");
            return;
        }
        encoded[encoded_length] = '\0';
        char chunk[kMaxFramePayload] = {};
        const int chunk_length = snprintf(chunk, sizeof(chunk), "%u\t%s",
                                          static_cast<unsigned>(offset),
                                          encoded);
        if (chunk_length <= 0 ||
            static_cast<size_t>(chunk_length) >= sizeof(chunk) ||
            !SendFrame(sequence, "DATA", chunk)) {
            heap_caps_free(pixels);
            return;
        }
    }
    heap_caps_free(pixels);
    SendFrame(sequence, "OK", "");
}
#else
void HandleInput(unsigned long sequence, char* const*, size_t) {
    SendFrame(sequence, "ERR", "test_control_disabled");
}

void SendScreenshot(unsigned long sequence, bool) {
    SendFrame(sequence, "ERR", "test_control_disabled");
}
#endif

void SendPackages(unsigned long sequence) {
#if CONFIG_PXA_ENABLED
    const size_t capacity =
        std::min(pxa_host_package_count(), kPxaPackageListCapacity);
    if (capacity == 0) {
        SendFrame(sequence, "OK", "");
        return;
    }
    auto* apps = static_cast<pxa_host_package_info_t*>(heap_caps_calloc(
        capacity, sizeof(pxa_host_package_info_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (apps == nullptr) {
        SendFrame(sequence, "ERR", "package_list_memory_unavailable");
        return;
    }

    const size_t count = pxa_host_list_packages(apps, capacity);
    bool sent = true;
    for (size_t index = 0; index < count; ++index) {
        const pxa_host_package_info_t& app = apps[index];
        char payload[kMaxFramePayload] = {};
        snprintf(payload, sizeof(payload), "%s\t%s\t%s\tbuiltin=%u;installed=%u;staged=%u;enabled=%u;active=%u",
                 app.id, app.name, app.version, static_cast<unsigned>(app.built_in),
                 static_cast<unsigned>(app.installed), static_cast<unsigned>(app.staged),
                 static_cast<unsigned>(app.enabled), static_cast<unsigned>(app.active));
        if (!SendFrame(sequence, "PKG", payload)) {
            sent = false;
            break;
        }
    }
    heap_caps_free(apps);
    if (sent) SendFrame(sequence, "OK", "");
#else
    SendFrame(sequence, "ERR", "pxa_disabled");
#endif
}

void HandlePackageAction(unsigned long sequence, const char* action, const char* identity) {
#if CONFIG_PXA_ENABLED
    if (!IsSafeIdentity(identity)) {
        SendFrame(sequence, "ERR", "invalid_identity");
        return;
    }
    pxa_host_app_action_t requested_action;
    if (strcmp(action, "install") == 0) {
        requested_action = PXA_HOST_APP_ACTION_INSTALL;
    } else if (strcmp(action, "uninstall") == 0) {
        requested_action = PXA_HOST_APP_ACTION_UNINSTALL;
    } else if (strcmp(action, "enable") == 0) {
        requested_action = PXA_HOST_APP_ACTION_ENABLE;
    } else if (strcmp(action, "disable") == 0) {
        requested_action = PXA_HOST_APP_ACTION_DISABLE;
    } else if (strcmp(action, "clear-data") == 0) {
        requested_action = PXA_HOST_APP_ACTION_CLEAR_DATA;
    } else {
        SendFrame(sequence, "ERR", "invalid_package_action");
        return;
    }
    if (!pxa_host_ready()) {
        SendFrame(sequence, "ERR", "pxa_unavailable");
    } else {
        const bool success = pxa_host_manage_app(requested_action, identity);
        SendFrame(sequence, success ? "OK" : "ERR", success ? "" : "package_action_failed");
    }
#else
    (void)action;
    (void)identity;
    SendFrame(sequence, "ERR", "pxa_disabled");
#endif
}

void HandlePackageDeploy(unsigned long sequence, const char* identity) {
#if CONFIG_PXA_ENABLED
    if (!IsSafeIdentity(identity)) {
        SendFrame(sequence, "ERR", "invalid_identity");
        return;
    }
    if (!pxa_host_ready()) {
        SendFrame(sequence, "ERR", "pxa_unavailable");
        return;
    }
    ESP_LOGI(kTag, "Package deploy started: %s stack_free=%uB", identity,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    const bool success = pxa_host_deploy_package(identity);
    ESP_LOGI(kTag, "Package deploy finished: %s success=%u stack_free=%uB",
             identity, static_cast<unsigned>(success),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    SendFrame(sequence, success ? "OK" : "ERR",
              success ? "" : "package_deploy_failed");
#else
    (void)identity;
    SendFrame(sequence, "ERR", "pxa_disabled");
#endif
}

void SendLogHistory(unsigned long sequence) {
    if (s_log_history == nullptr) return;
    size_t start = 0;
    size_t count = 0;
    portENTER_CRITICAL(&s_history_lock);
    start = s_log_history_start;
    count = s_log_history_count;
    portEXIT_CRITICAL(&s_history_lock);
    for (size_t index = 0; index < count; ++index) {
        LogRecord record = {};
        portENTER_CRITICAL(&s_history_lock);
        record = s_log_history[(start + index) % kLogHistoryDepth];
        portEXIT_CRITICAL(&s_history_lock);
        char payload[kMaxLogFramePayload] = {};
        const int length = snprintf(
            payload, sizeof(payload), "%u\t%.*s",
            static_cast<unsigned>(record.timestamp_ms),
            static_cast<int>(sizeof(record.message) - 1u), record.message);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload) ||
            !SendFrame(sequence, "LOG", payload))
            return;
    }
}

void HandleCommand(char* line) {
    char* save = nullptr;
    char* protocol = strtok_r(line, " ", &save);
    char* sequence_text = strtok_r(nullptr, " ", &save);
    char* command = strtok_r(nullptr, " ", &save);
    char* arguments[10] = {};
    size_t argument_count = 0;
    while (argument_count < sizeof(arguments) / sizeof(arguments[0])) {
        char* value = strtok_r(nullptr, " ", &save);
        if (value == nullptr) break;
        arguments[argument_count++] = value;
    }
    char* argument = argument_count > 0 ? arguments[0] : nullptr;
    char* second_argument = argument_count > 1 ? arguments[1] : nullptr;
    char* third_argument = argument_count > 2 ? arguments[2] : nullptr;
    if (protocol == nullptr || sequence_text == nullptr || command == nullptr ||
        strcmp(protocol, kProtocol) != 0) {
        return;
    }
    char* end = nullptr;
    const unsigned long sequence = strtoul(sequence_text, &end, 10);
    if (end == sequence_text || *end != '\0') return;
    s_last_control_activity_us.store(
        static_cast<uint64_t>(esp_timer_get_time()), std::memory_order_relaxed);

    if (strcmp(command, "HELLO") == 0) {
        SendHello(sequence);
        if (!s_control_session_active.exchange(true)) {
            ESP_LOGI(kTag, "PXADB control session connected");
        }
    } else if (strcmp(command, "BYE") == 0) {
#if CONFIG_PXADB_TEST_CONTROL
        QueueSessionCancel();
#endif
        s_control_session_active.store(false);
        SendFrame(sequence, "OK", "disconnected");
        ESP_LOGI(kTag, "PXADB control session disconnected");
    } else if (strcmp(command, "PING") == 0) {
        SendFrame(sequence, "OK", "pong");
    } else if (strcmp(command, "INFO") == 0) {
        SendInfo(sequence);
    } else if (strcmp(command, "LOGSUB") == 0) {
        s_dropped_logs.store(0);
        EnableLogCapture();
        SendLogHistory(sequence);
        SendFrame(sequence, "OK", "subscribed");
    } else if (strcmp(command, "LOGUNSUB") == 0) {
        SendFrame(sequence, "OK", "unsubscribed");
        DisableLogCapture();
    } else if (strcmp(command, "PACKAGES") == 0) {
        SendPackages(sequence);
        ESP_LOGI(kTag, "Package list finished: stack_free=%uB",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    } else if (strcmp(command, "INPUT") == 0) {
        HandleInput(sequence, arguments, argument_count);
    } else if (strcmp(command, "SCREENSHOT") == 0) {
        const bool legacy_quality = argument != nullptr &&
            argument[0] >= '0' && argument[0] <= '9';
        if (argument_count > 1 ||
            (argument != nullptr && !legacy_quality &&
             strcmp(argument, "AFTER_PRESENT") != 0)) {
            SendFrame(sequence, "ERR", "invalid_screenshot_mode");
        } else {
            SendScreenshot(sequence, argument != nullptr &&
                                     strcmp(argument, "AFTER_PRESENT") == 0);
        }
    } else if (strcmp(command, "FSLIST") == 0 && argument != nullptr) {
        SendFsList(sequence, argument);
    } else if (strcmp(command, "FSGET") == 0 && argument != nullptr) {
        SendFsFile(sequence, argument);
    } else if (strcmp(command, "FSPUT") == 0 && argument != nullptr && second_argument != nullptr) {
        StartFsUpload(sequence, argument, second_argument, third_argument);
    } else if (strcmp(command, "FSDATA") == 0 && argument != nullptr) {
        WriteFsUpload(sequence, argument, second_argument);
    } else if (strcmp(command, "FSMKDIR") == 0 && argument != nullptr) {
        MakeFsDirectory(sequence, argument);
    } else if (strcmp(command, "FSRM") == 0 && argument != nullptr) {
        RemoveFsPath(sequence, argument);
    } else if (strcmp(command, "PACKAGE") == 0 && argument != nullptr &&
               strcmp(argument, "deploy") == 0 && second_argument != nullptr) {
        HandlePackageDeploy(sequence, second_argument);
    } else if (strcmp(command, "PACKAGE") == 0 && argument != nullptr && second_argument != nullptr) {
        HandlePackageAction(sequence, argument, second_argument);
    } else if (strcmp(command, "REBOOT") == 0) {
        SendFrame(sequence, "OK", "rebooting");
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_restart();
    } else {
        SendFrame(sequence, "ERR", "unknown_command");
    }
}

void SendPendingLogs() {
    if (!s_log_subscribed.load() || s_log_queue == nullptr) return;
    const uint32_t dropped = s_dropped_logs.exchange(0);
    if (dropped > 0) {
        char payload[64] = {};
        snprintf(payload, sizeof(payload), "dropped=%u", static_cast<unsigned>(dropped));
        SendFrame(0, "DROP", payload);
    }
    for (int index = 0; index < 4; ++index) {
        LogRecord record = {};
        if (xQueueReceive(s_log_queue, &record, 0) != pdTRUE) break;
        char payload[kMaxLogFramePayload] = {};
        const int length = snprintf(
            payload, sizeof(payload), "%u\t%.*s",
            static_cast<unsigned>(record.timestamp_ms),
            static_cast<int>(sizeof(record.message) - 1u), record.message);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload) ||
            !SendFrame(0, "LOG", payload))
            break;
    }
}

#if CONFIG_PXADB_TEST_CONTROL
bool TakeInput(InputCommand* command) {
    bool taken = false;
    portENTER_CRITICAL(&s_input_lock);
    taken = s_input_mailbox.Take(command);
    portEXIT_CRITICAL(&s_input_lock);
    return taken;
}

void ResetAcceptedPointer() {
    portENTER_CRITICAL(&s_input_lock);
    s_accepted_pointer.Reset();
    portEXIT_CRITICAL(&s_input_lock);
}

bool ExecutePointer(PointerState* state, bool* delivered,
                    PointerAction action, uint16_t x,
                    uint16_t y, uint8_t pointer_id) {
    if (!TestControlAvailable() || state == nullptr || delivered == nullptr ||
        !state->Apply(action, pointer_id)) {
        return false;
    }
    if (action == PointerAction::kDown) {
        *delivered = s_test_control.route_pointer_down(s_test_control.context);
        return !*delivered || s_test_control.inject_pointer(
                                  s_test_control.context, x, y, true);
    }
    if (action == PointerAction::kMove) {
        return !*delivered || s_test_control.inject_pointer(
                                  s_test_control.context, x, y, true);
    }
    bool success = true;
    if (*delivered) {
        success = action == PointerAction::kCancel
                      ? s_test_control.cancel_pointer(s_test_control.context)
                      : s_test_control.inject_pointer(s_test_control.context,
                                                      x, y, false);
    }
    *delivered = false;
    return success;
}

void DelayUntil(TickType_t target) {
    constexpr TickType_t kMaximumSlice = pdMS_TO_TICKS(20);
    while (s_running.load()) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= target) return;
        const TickType_t remaining = target - now;
        vTaskDelay(remaining < kMaximumSlice ? remaining : kMaximumSlice);
    }
}

bool ExecuteInput(const InputCommand& command, PointerState* pointer,
                  bool* delivered) {
    if (command.kind == InputCommandKind::kPointer) {
        return ExecutePointer(pointer, delivered,
                              command.pointer_action, command.x, command.y,
                              command.pointer_id);
    }
    if (command.kind == InputCommandKind::kCancel) {
        if (!pointer->pressed()) return true;
        return ExecutePointer(pointer, delivered,
                              PointerAction::kCancel, 0, 0,
                              pointer->pointer_id());
    }
    if (command.kind == InputCommandKind::kKey) {
        if (pointer->pressed() &&
            !ExecutePointer(pointer, delivered,
                            PointerAction::kCancel, 0, 0,
                            pointer->pointer_id())) {
            return false;
        }
        TestControlKey key = TestControlKey::kBack;
        switch (command.key) {
            case InputKey::kBack:
                key = TestControlKey::kBack;
                break;
            case InputKey::kHome:
                key = TestControlKey::kHome;
                break;
            case InputKey::kVolumeUp:
                key = TestControlKey::kVolumeUp;
                break;
            case InputKey::kVolumeDown:
                key = TestControlKey::kVolumeDown;
                break;
        }
        return s_test_control.route_key(s_test_control.context, key);
    }
    if (command.kind == InputCommandKind::kSync) return true;

    if (pointer->pressed() ||
        !ExecutePointer(pointer, delivered, PointerAction::kDown,
                        command.x, command.y, command.pointer_id)) {
        return false;
    }
    if (command.kind == InputCommandKind::kTap) {
        const TickType_t target = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(command.duration_ms);
        DelayUntil(target);
        return ExecutePointer(pointer, delivered, PointerAction::kUp,
                              command.x, command.y, command.pointer_id);
    }

    const TickType_t started = xTaskGetTickCount();
    for (uint16_t step = 1; step <= command.steps && s_running.load(); ++step) {
        const TickType_t target = started + pdMS_TO_TICKS(
            static_cast<uint32_t>(command.duration_ms) * step / command.steps);
        DelayUntil(target);
        const uint16_t x = InterpolateCoordinate(
            command.x, command.end_x, step, command.steps);
        const uint16_t y = InterpolateCoordinate(
            command.y, command.end_y, step, command.steps);
        if (!ExecutePointer(pointer, delivered, PointerAction::kMove,
                            x, y, command.pointer_id)) {
            return false;
        }
    }
    return ExecutePointer(pointer, delivered, PointerAction::kUp,
                          command.end_x, command.end_y, command.pointer_id);
}

void PxadbInputTask(void*) {
    PointerState pointer;
    bool delivered = false;
    uint64_t last_activity_us = static_cast<uint64_t>(esp_timer_get_time());
    while (s_running.load()) {
        (void)xSemaphoreTake(s_input_signal, pdMS_TO_TICKS(50));
        InputCommand command;
        while (s_running.load() && TakeInput(&command)) {
            const bool success = ExecuteInput(command, &pointer, &delivered);
            last_activity_us = static_cast<uint64_t>(esp_timer_get_time());
            SendInputAck(command, success ? "processed" : "rejected");
            if (!success && pointer.pressed()) {
                (void)ExecutePointer(&pointer, &delivered,
                                     PointerAction::kCancel, 0, 0,
                                     pointer.pointer_id());
                ResetAcceptedPointer();
            }
        }
        const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
        if (pointer.pressed() &&
            now_us - last_activity_us >=
                static_cast<uint64_t>(kInputTimeoutMs) * 1000u) {
            (void)ExecutePointer(&pointer, &delivered,
                                 PointerAction::kCancel, 0, 0,
                                 pointer.pointer_id());
            ResetAcceptedPointer();
            ESP_LOGW(kTag, "Synthetic pointer cancelled after input timeout");
        }
    }
    if (pointer.pressed()) {
        (void)ExecutePointer(&pointer, &delivered,
                             PointerAction::kCancel, 0, 0,
                             pointer.pointer_id());
    }
    ResetAcceptedPointer();
    s_input_task_active.store(false);
    vTaskDelete(nullptr);
}
#endif

void PxadbTask(void*) {
    char command[kMaxCommandLength] = {};
    size_t command_length = 0;
    bool discarding_command = false;
    uint8_t input[128] = {};
    while (s_running.load()) {
        const int received = TransportRead(input, sizeof(input), kTransportReadPollMs);
        if (received <= 0) {
            SendPendingLogs();
            const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
            const uint64_t last_us = s_last_control_activity_us.load(
                std::memory_order_relaxed);
            if (s_control_session_active.load() && last_us != 0 &&
                now_us - last_us >=
                    static_cast<uint64_t>(kSessionIdleTimeoutMs) * 1000u) {
                s_control_session_active.store(false);
#if CONFIG_PXADB_TEST_CONTROL
                QueueSessionCancel();
#endif
                ESP_LOGI(kTag, "PXADB control session timed out");
            }
            continue;
        }
        for (int index = 0; index < received; ++index) {
            const char character = static_cast<char>(input[index]);
            if (character == '\r') continue;
            if (character == '\n') {
                if (!discarding_command && command_length > 0) {
                    command[command_length] = '\0';
                    HandleCommand(command);
                }
                command_length = 0;
                discarding_command = false;
            } else if (discarding_command) {
                continue;
            } else if (command_length + 1 < sizeof(command)) {
                command[command_length++] = character;
            } else {
                command_length = 0;
                discarding_command = true;
            }
        }
        SendPendingLogs();
    }
    if (s_control_session_active.exchange(false)) {
#if CONFIG_PXADB_TEST_CONTROL
        QueueSessionCancel();
#endif
        ESP_LOGI(kTag, "PXADB control session stopped");
    }
    s_task_active.store(false);
    vTaskDelete(nullptr);
}

void PxadbAutostartTask(void*) {
    esp_err_t result = ESP_FAIL;
    for (unsigned attempt = 1;
         attempt <= 5 && !s_autostart_cancelled.load(); ++attempt) {
        result = Start();
        if (result == ESP_OK) break;
        ESP_LOGW(kTag, "PXADB autostart attempt %u/5 failed: %s",
                 attempt, esp_err_to_name(result));
        if (attempt != 5) {
            vTaskDelay(pdMS_TO_TICKS(250u << (attempt - 1)));
        }
    }
    if (result != ESP_OK && !s_autostart_cancelled.load()) {
        ESP_LOGE(kTag, "PXADB autostart exhausted retries: %s",
                 esp_err_to_name(result));
    }
    s_autostart_active.store(false);
    vTaskDelete(nullptr);
}

void CleanupService(bool cancel_autostart) {
    if (cancel_autostart) s_autostart_cancelled.store(true);
    if (!s_running.exchange(false)) return;
#if CONFIG_PXADB_TEST_CONTROL
    if (s_test_control.set_input_takeover_callback != nullptr) {
        s_test_control.set_input_takeover_callback(s_test_control.context,
                                                   nullptr, nullptr);
    }
#endif
    DisableLogCapture();
#if CONFIG_PXADB_TEST_CONTROL
    if (s_input_signal != nullptr) xSemaphoreGive(s_input_signal);
#endif
    const TickType_t started = xTaskGetTickCount();
    while ((s_task_active.load()
#if CONFIG_PXADB_TEST_CONTROL
            || s_input_task_active.load()
#endif
            ) &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(3500)) {
        vTaskDelay(1);
    }
    const bool tasks_stopped = !s_task_active.load()
#if CONFIG_PXADB_TEST_CONTROL
                               && !s_input_task_active.load()
#endif
        ;
    if (!tasks_stopped) {
        ESP_LOGE(kTag, "PXADB tasks did not stop; retaining synchronization objects");
        return;
    }
    if (s_log_queue != nullptr) {
        vQueueDelete(s_log_queue);
        s_log_queue = nullptr;
    }
    if (s_tx_mutex != nullptr) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = nullptr;
    }
#if CONFIG_PXADB_TEST_CONTROL
    if (s_input_signal != nullptr) {
        vSemaphoreDelete(s_input_signal);
        s_input_signal = nullptr;
    }
    portENTER_CRITICAL(&s_input_lock);
    s_input_mailbox.Clear();
    s_accepted_pointer.Reset();
    portEXIT_CRITICAL(&s_input_lock);
#endif
    AbortFileUpload();
}

}  // namespace

esp_err_t ConfigureTestControl(const TestControlAdapter* adapter) {
#if CONFIG_PXADB_TEST_CONTROL
    if (s_running.load()) return ESP_ERR_INVALID_STATE;
    if (adapter == nullptr) {
        s_test_control = {};
        return ESP_OK;
    }
    if (adapter->struct_size < sizeof(TestControlAdapter) ||
        adapter->width == 0 || adapter->height == 0 ||
        adapter->route_pointer_down == nullptr ||
        adapter->inject_pointer == nullptr ||
        adapter->cancel_pointer == nullptr || adapter->route_key == nullptr ||
        adapter->capture_rgb565 == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    s_test_control = *adapter;
    return ESP_OK;
#else
    (void)adapter;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t Start() {
    if (s_running.load()) return ESP_OK;
    const esp_err_t transport_err = TransportStart();
    if (transport_err != ESP_OK) return transport_err;
    // Log history can later be read by LOGSUB. Keep it in internal RAM because
    // package and filesystem operations can temporarily disable the flash cache.
    if (s_log_history == nullptr) {
        s_log_history = static_cast<LogRecord*>(heap_caps_calloc(
            kLogHistoryDepth, sizeof(LogRecord),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (s_log_history == nullptr) {
            ESP_LOGW(kTag, "PXADB log history is unavailable; live logcat remains enabled");
        }
    }
    portENTER_CRITICAL(&s_history_lock);
    s_log_history_start = 0;
    s_log_history_count = 0;
    portEXIT_CRITICAL(&s_history_lock);
    s_log_queue = xQueueCreateWithCaps(
        kLogQueueDepth, sizeof(LogRecord),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_log_queue == nullptr) return ESP_ERR_NO_MEM;
    s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_storage);
    if (s_tx_mutex == nullptr) {
        vQueueDelete(s_log_queue);
        s_log_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_PXADB_TEST_CONTROL
    s_input_signal = xSemaphoreCreateBinaryStatic(&s_input_signal_storage);
    if (s_input_signal == nullptr) {
        vQueueDelete(s_log_queue);
        s_log_queue = nullptr;
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_input_lock);
    s_input_mailbox.Clear();
    s_accepted_pointer.Reset();
    portEXIT_CRITICAL(&s_input_lock);
#endif
    s_running.store(true);
#if CONFIG_PXADB_TEST_CONTROL
    if (s_test_control.set_input_takeover_callback != nullptr) {
        s_test_control.set_input_takeover_callback(
            s_test_control.context, HandleSystemInputTakeover, nullptr);
    }
#endif
#if CONFIG_PXADB_TEST_CONTROL
    s_input_task_active.store(true);
    if (xTaskCreateWithCaps(PxadbInputTask, "pxadb-input",
                            kInputTaskStackSize, nullptr, 5, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_input_task_active.store(false);
        CleanupService(false);
        return ESP_ERR_NO_MEM;
    }
#endif
    s_task_active.store(true);
    if (xTaskCreateWithCaps(PxadbTask, "pxadbd", kTaskStackSize, nullptr, 4,
                            nullptr, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_task_active.store(false);
        CleanupService(false);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "PXADB service ready: control_stack=%uB internal; free_sram=%u largest_sram=%u",
             static_cast<unsigned>(kTaskStackSize),
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return ESP_OK;
}

esp_err_t StartAutostart() {
    if (s_running.load() || s_autostart_active.exchange(true)) return ESP_OK;
    s_autostart_cancelled.store(false);
    if (xTaskCreateWithCaps(PxadbAutostartTask, "pxadb-start",
                            kAutostartTaskStackSize, nullptr, 3, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_autostart_active.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Stop() {
    CleanupService(true);
}

bool IsRunning() {
    return s_running.load();
}

bool IsEnabled() {
    return IsRunning();
}

bool SetEnabled(bool enabled) {
    if (!enabled) {
        Stop();
        return true;
    }

    const esp_err_t err = Start();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Unable to enable PXADB: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace pxadb

#else

namespace pxadb {
esp_err_t ConfigureTestControl(const TestControlAdapter*) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t Start() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StartAutostart() { return ESP_ERR_NOT_SUPPORTED; }
void Stop() {}
bool IsRunning() { return false; }
bool IsEnabled() { return false; }
bool SetEnabled(bool enabled) {
    (void)enabled;
    return false;
}
}  // namespace pxadb

#endif
