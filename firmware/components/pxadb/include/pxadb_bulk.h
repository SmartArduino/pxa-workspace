#ifndef PXADB_BULK_H
#define PXADB_BULK_H

#include <cstddef>
#include <cstdint>

namespace pxadb {
// COBS gives raw uploads an unambiguous terminator even after truncation.
// Decode in place: the write cursor never overtakes unread encoded bytes.
inline size_t decode_bulk_size(uint8_t* bytes, size_t encoded, size_t capacity) {
    size_t read = 0, write = 0;
    if (!bytes || !encoded || !capacity) return 0;
    while (read < encoded) {
        const unsigned code = bytes[read++];
        if (!code || code - 1 > encoded - read || code - 1 > capacity - write)
            return 0;
        for (unsigned i = 1; i < code; ++i) {
            if (!bytes[read]) return 0;
            bytes[write++] = bytes[read++];
        }
        if (code != 255 && read < encoded) {
            if (write == capacity) return 0;
            bytes[write++] = 0;
        }
    }
    return write;
}
inline bool decode_bulk(uint8_t* bytes, size_t encoded, size_t expected) {
    return expected && decode_bulk_size(bytes, encoded, expected) == expected;
}

// Fixed little-endian header; CRC covers the header and data together.
inline bool parse_fast(uint8_t* bytes, size_t size, size_t limit,
                       uint32_t* sequence, uint64_t* offset, size_t* length) {
    if (!bytes || size < 24 || bytes[0]!='P' || bytes[1]!='X' ||
        bytes[2]!='D' || bytes[3]!='1') return false;
    *sequence=0; *offset=0; *length=0;
    for(unsigned i=0;i<4;++i) *sequence |= uint32_t(bytes[4+i])<<(i*8);
    for(unsigned i=0;i<8;++i) *offset |= uint64_t(bytes[8+i])<<(i*8);
    for(unsigned i=0;i<4;++i) *length |= size_t(bytes[16+i])<<(i*8);
    return *length && *length<=limit && size==24+*length;
}

inline size_t fast_chunk_limit(size_t requested, size_t free_bytes, size_t largest) {
    // 3840 + 24 header/CRC + COBS fits inside a 4096-byte RX ring.
    size_t size=requested<3840?requested:3840;
    // Leave at least 3/4 of the available heap to the runtime and filesystem.
    for(;size>=256;size=size>512?size/2:256) {
        const size_t raw=size+24, encoded=raw+raw/254+1;
        if(encoded<=free_bytes/4 && encoded<=largest)return size;
        if(size==256)break;
    }
    return 0;
}
}
#endif
