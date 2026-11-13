#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace pxadb {

enum class Transport : uint8_t { kPrimary, kUart };

esp_err_t TransportStart(Transport transport);
int TransportRead(Transport transport, uint8_t* bytes, size_t size, uint32_t timeout_ms);
int TransportWrite(Transport transport, const uint8_t* bytes, size_t size);
bool TransportTxDone(Transport transport);
size_t TransportWriteChunk(Transport transport);
const char* TransportName(Transport transport);
bool TransportIsUart(Transport transport);
unsigned TransportUartRxCapacity();
int TransportUartRxFifoThreshold();
unsigned TransportBaudRate(Transport transport);

}  // namespace pxadb
