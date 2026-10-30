// SPDX-License-Identifier: Apache-2.0
#include "watcher_vision_reply.h"
#include <mbedtls/base64.h>
#include <cJSON.h>
#include <cstring>

// Validate string boundaries/depth before cJSON, and remove the large base64
// value in place. Only the small remaining metadata goes through cJSON.
// A malformed/escaped image is rejected, never interpreted as another command.
pxa_status_t WatcherDecodeVisionReply(char* line, size_t length, uint16_t format,
                        uint8_t* output, size_t capacity, size_t* size) {
    unsigned depth = 0;
    char* image = nullptr;
    size_t image_size = 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = line[i];
        if (c == '"') {
            const auto start = ++i;
            bool escaped = false;
            for (; i < length && line[i] != '"'; ++i) {
                if (static_cast<unsigned char>(line[i]) < 0x20) return PXA_STATUS_PROTOCOL_ERROR;
                if (line[i] == '\\') { escaped = true; if (++i == length) return PXA_STATUS_PROTOCOL_ERROR; }
            }
            if (i == length) return PXA_STATUS_PROTOCOL_ERROR;
            if (depth == 2 && !escaped && i - start == 5 && !memcmp(line + start, "image", 5)) {
                size_t j = i + 1;
                while (j < length && (line[j] == ' ' || line[j] == '\t')) ++j;
                if (j < length && line[j] == ':') {
                    ++j; while (j < length && (line[j] == ' ' || line[j] == '\t')) ++j;
                    if (j == length || line[j++] != '"' || image) return PXA_STATUS_PROTOCOL_ERROR;
                    const auto begin = j;
                    while (j < length && line[j] != '"') {
                        const char x = line[j++];
                        if (!((x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') ||
                              (x >= '0' && x <= '9') || x == '+' || x == '/' || x == '='))
                            return PXA_STATUS_PROTOCOL_ERROR;
                    }
                    if (j == length) return PXA_STATUS_PROTOCOL_ERROR;
                    image = line + begin; image_size = j - begin; i = j;
                }
            }
        } else if (c == '{' || c == '[') {
            if (++depth > 12) return PXA_STATUS_PROTOCOL_ERROR;
        } else if (c == '}' || c == ']') {
            if (!depth) return PXA_STATUS_PROTOCOL_ERROR;
            --depth;
        }
    }
    if (depth) return PXA_STATUS_PROTOCOL_ERROR;
    size_t decoded = 0;
    if (image) {
        if (format == PXA_VISION_JSON && image_size) return PXA_STATUS_PROTOCOL_ERROR;
        if (format == PXA_VISION_JPEG && (!image_size || image_size % 4 ||
            mbedtls_base64_decode(output, capacity, &decoded,
                reinterpret_cast<const unsigned char*>(image), image_size))) return PXA_STATUS_PROTOCOL_ERROR;
        const auto offset = size_t(image - line);
        memmove(image, image + image_size, length - offset - image_size + 1);
        length -= image_size;
    }
    if (length > PXA_VISION_MAX_JSON) return PXA_STATUS_LIMIT_EXCEEDED;
    cJSON* root = cJSON_ParseWithLengthOpts(line, length + 1, nullptr, true);
    if (!root) return PXA_STATUS_PROTOCOL_ERROR;
    const auto* name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const auto* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char* expected = format == PXA_VISION_JPEG ? "SAMPLE" : "INVOKE";
    pxa_status_t result = PXA_STATUS_WOULD_BLOCK;
    if (cJSON_IsString(name) && !strcmp(name->valuestring, expected)) {
        if (!cJSON_IsNumber(code) || !cJSON_IsNumber(type)) result = PXA_STATUS_PROTOCOL_ERROR;
        else if (code->valueint) result = PXA_STATUS_UNAVAILABLE;
        else if (type->valueint == 1 && cJSON_IsObject(data)) {
            if (format == PXA_VISION_JPEG) {
                const auto* value = cJSON_GetObjectItemCaseSensitive(data, "image");
                // Stock 2024.08.16 Himax firmware includes zero DMA padding
                // after EOI (32 bytes on the attached unit). Canonical JPEG
                // frames omit at most one 64-byte alignment block of zeros.
                const auto padded = decoded;
                while (decoded > 2 && output[decoded - 1] == 0 && padded - decoded < 64) --decoded;
                if (!image || !cJSON_IsString(value) || *value->valuestring || decoded < 4 ||
                    output[0] != 0xff || output[1] != 0xd8 || output[decoded - 2] != 0xff || output[decoded - 1] != 0xd9)
                    result = PXA_STATUS_PROTOCOL_ERROR;
                else { *size = decoded; result = PXA_STATUS_OK; }
            } else if (length <= capacity) {
                memcpy(output, line, length); *size = length; result = PXA_STATUS_OK;
            } else result = PXA_STATUS_LIMIT_EXCEEDED;
        }
    }
    cJSON_Delete(root);
    return result;
}
