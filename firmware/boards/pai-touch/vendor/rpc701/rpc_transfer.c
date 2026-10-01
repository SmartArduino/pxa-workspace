#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "rpc_701_internal.h"

static const char* TAG = "RPC_TRANSFER";

typedef enum {
    PARSE_STATE_WAIT_HEADER1 = 0,
    PARSE_STATE_WAIT_HEADER2,
    PARSE_STATE_WAIT_TYPE,
    PARSE_STATE_WAIT_LEN_H,
    PARSE_STATE_WAIT_LEN_L,
    PARSE_STATE_WAIT_DATA,
    PARSE_STATE_WAIT_CRC,
    PARSE_STATE_VERIFY_CRC,
} parse_state_t;

typedef struct {
    parse_state_t state;
    uint8_t type;
    uint16_t length;
    uint16_t data_received;
    uint8_t *data_buffer;
    uint8_t frame_buffer[SERIAL_PROTO_MAX_PACK_LEN];
    uint16_t frame_pos;
    uint32_t error_count;
    uint32_t success_count;
} proto_parser_t;

static proto_parser_t s_parser = {0};

static transfer_write_cb_t s_write_cb = NULL;
static transfer_recv_cb_t s_recv_cb = NULL;
static void *s_user_ctx = NULL;

static uint8_t crc8_calculate(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            crc = crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1;
        }
    }
    return crc;
}

static void parser_reset(proto_parser_t *parser)
{
    parser->state = PARSE_STATE_WAIT_HEADER1;
    parser->type = 0;
    parser->length = 0;
    parser->data_received = 0;
    parser->frame_pos = 0;
    if (parser->data_buffer) {
        free(parser->data_buffer);
        parser->data_buffer = NULL;
    }
}

static int parser_process_byte(proto_parser_t *parser, uint8_t byte)
{
    switch (parser->state) {
        case PARSE_STATE_WAIT_HEADER1:
            if (byte == SERIAL_PROTO_HEADER_H) {
                parser->frame_buffer[parser->frame_pos++] = byte;
                parser->state = PARSE_STATE_WAIT_HEADER2;
            }
            break;
            
        case PARSE_STATE_WAIT_HEADER2:
            if (byte == SERIAL_PROTO_HEADER_L) {
                parser->frame_buffer[parser->frame_pos++] = byte;
                parser->state = PARSE_STATE_WAIT_TYPE;
            } else {
                parser->error_count++;
                parser_reset(parser);
                if (byte == SERIAL_PROTO_HEADER_H) {
                    parser->frame_buffer[parser->frame_pos++] = byte;
                    parser->state = PARSE_STATE_WAIT_HEADER2;
                }
            }
            break;
            
        case PARSE_STATE_WAIT_TYPE:
            parser->type = byte;
            parser->frame_buffer[parser->frame_pos++] = byte;
            parser->state = PARSE_STATE_WAIT_LEN_H;
            break;
            
        case PARSE_STATE_WAIT_LEN_H:
            parser->length = byte << 8;
            parser->frame_buffer[parser->frame_pos++] = byte;
            parser->state = PARSE_STATE_WAIT_LEN_L;
            break;
            
        case PARSE_STATE_WAIT_LEN_L:
            parser->length |= byte;
            parser->frame_buffer[parser->frame_pos++] = byte;
            
            if (parser->length > SERIAL_PROTO_MAX_DATA_LEN) {
                ESP_LOGW(TAG, "Invalid packet length: %d", parser->length);
                parser->error_count++;
                parser_reset(parser);
                return -1;
            }
            
            if (parser->length == 0) {
                parser->state = PARSE_STATE_WAIT_CRC;
            } else {
                if (parser->data_buffer) {
                    free(parser->data_buffer);
                }
                parser->data_buffer = (uint8_t *)malloc(parser->length);
                if (!parser->data_buffer) {
                    ESP_LOGE(TAG, "Failed to allocate buffer for data");
                    parser_reset(parser);
                    return -1;
                }
                parser->data_received = 0;
                parser->state = PARSE_STATE_WAIT_DATA;
            }
            break;
            
        case PARSE_STATE_WAIT_DATA:
            parser->data_buffer[parser->data_received++] = byte;
            parser->frame_buffer[parser->frame_pos++] = byte;
            if (parser->data_received >= parser->length) {
                parser->state = PARSE_STATE_WAIT_CRC;
            }
            break;
            
        case PARSE_STATE_WAIT_CRC:
            parser->frame_buffer[parser->frame_pos++] = byte;
            parser->state = PARSE_STATE_VERIFY_CRC;
            return 1;
            
        default:
            parser_reset(parser);
            break;
    }
    return 0;
}

static int parser_verify_crc(proto_parser_t *parser)
{
    uint8_t calculated_crc = crc8_calculate(parser->frame_buffer, parser->frame_pos - 1);
    uint8_t received_crc = parser->frame_buffer[parser->frame_pos - 1];
    
    if (calculated_crc == received_crc) {
        parser->success_count++;
        return 0;
    } else {
        ESP_LOGW(TAG, "CRC mismatch: calc=0x%02x, recv=0x%02x", calculated_crc, received_crc);
        parser->error_count++;
        return -1;
    }
}

void serial_transfer_init(transfer_write_cb_t write_cb, transfer_recv_cb_t recv_cb, void *user_ctx)
{
    s_write_cb = write_cb;
    s_recv_cb = recv_cb;
    s_user_ctx = user_ctx;

    memset(&s_parser, 0, sizeof(s_parser));
    s_parser.state = PARSE_STATE_WAIT_HEADER1;
}

void serial_transfer_deinit(void)
{
    if (s_parser.data_buffer) {
        free(s_parser.data_buffer);
        s_parser.data_buffer = NULL;
    }

    s_write_cb = NULL;
    s_recv_cb = NULL;
    s_user_ctx = NULL;

    memset(&s_parser, 0, sizeof(s_parser));
}

void serial_transfer_process(uint8_t *data, size_t len)
{

    for (size_t i = 0; i < len; i++)
    {
        int ret = parser_process_byte(&s_parser, data[i]);
        if (ret == 1) {
            // CRC byte received, verify it
            if (parser_verify_crc(&s_parser) == 0) {
                // CRC OK, deliver to application
                if (s_recv_cb) {
                    s_recv_cb(s_parser.type, s_parser.data_buffer, s_parser.length, s_user_ctx);
                }
            }
            // Reset parser for next frame
            parser_reset(&s_parser);
        }
    }
}

void serial_transfer_send(const void *data, int len)
{
    if (s_write_cb) {
        s_write_cb((const uint8_t *)data, len, s_user_ctx);
    }
}

void serial_transfer_reset(){
    parser_reset(&s_parser);
}

int serial_transfer_send_packed(serial_pack_type_t type, const void *data, int len)
{
    if (len > SERIAL_PROTO_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Data too large: %d", len);
        return -1;
    }

    size_t frame_size = 5 + len + 1; // header(2) + type(1) + length(2) + data + crc(1)
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    if (!frame) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        return -1;
    }

    uint16_t pos = 0;
    frame[pos++] = SERIAL_PROTO_HEADER_H;
    frame[pos++] = SERIAL_PROTO_HEADER_L;
    frame[pos++] = (uint8_t)type;
    frame[pos++] = (len >> 8) & 0xFF;
    frame[pos++] = len & 0xFF;

    if (data && len > 0) {
        memcpy(&frame[pos], data, len);
        pos += len;
    }

    uint8_t crc = crc8_calculate(frame, pos);
    frame[pos++] = crc;

    serial_transfer_send(frame, pos);
    free(frame);
    return pos;
}