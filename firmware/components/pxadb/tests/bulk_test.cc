#include "pxadb_bulk.h"
#include <algorithm>
#include <cassert>
#include <random>
#include <vector>

static std::vector<uint8_t> encode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> output(1);
    size_t start = 0;
    for (uint8_t byte : input) {
        if (byte == 0 || output.size() - start == 255) {
            output[start] = output.size() - start;
            start = output.size(); output.push_back(0);
            if (byte == 0) continue;
        }
        output.push_back(byte);
    }
    output[start] = output.size() - start;
    return output;
}

int main() {
    assert(pxadb::fast_chunk_limit(2048, 65536, 4096)==2048);
    assert(pxadb::fast_chunk_limit(2048, 4096, 4096)==512);
    assert(pxadb::fast_chunk_limit(2048, 65536, 300)==256);
    assert(pxadb::fast_chunk_limit(2048, 1000, 1000)==0);
    assert(pxadb::fast_chunk_limit(3840, 65536, 4096)==3840);
    assert(pxadb::fast_chunk_limit(3840, 1200, 300)==256);
    std::vector<uint8_t> frame(280,0);
    frame[0]='P';frame[1]='X';frame[2]='D';frame[3]='1';
    frame[4]=123;frame[8]=128;frame[17]=1;
    uint32_t sequence;uint64_t offset;size_t length;
    assert(pxadb::parse_fast(frame.data(),frame.size(),256,&sequence,&offset,&length));
    assert(sequence==123 && offset==128 && length==256);
    assert(!pxadb::parse_fast(frame.data(),279,256,&sequence,&offset,&length));
    assert(!pxadb::parse_fast(frame.data(),280,255,&sequence,&offset,&length));
    std::mt19937 random(42);
    for (size_t size : {1u, 2u, 253u, 254u, 255u, 256u, 1024u, 16384u}) {
        for (unsigned pattern = 0; pattern < 4; ++pattern) {
            std::vector<uint8_t> original(size);
            for (size_t i = 0; i < size; ++i)
                original[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : pattern == 2 ? i : random();
            auto bytes = encode(original);
            assert(pxadb::decode_bulk(bytes.data(), bytes.size(), size));
            assert(std::equal(original.begin(), original.end(), bytes.begin()));
            bytes = encode(original);
            assert(!pxadb::decode_bulk(bytes.data(), bytes.size(), size + 1));
            bytes = encode(original);
            assert(!pxadb::decode_bulk(bytes.data(), bytes.size(), size - 1));
        }
    }
    // Reject truncated runs, embedded delimiters and zero length. Fuzz bounds
    // under ASan/UBSan; invalid frames must never access outside their body.
    for (std::vector<uint8_t> bytes : std::vector<std::vector<uint8_t>>{{0}, {4, 1, 2}, {3, 0, 1}, {255, 1}})
        assert(!pxadb::decode_bulk(bytes.data(), bytes.size(), 16));
    for (unsigned trial = 0; trial < 10000; ++trial) {
        std::vector<uint8_t> bytes(random() % 512 + 1);
        for (auto& byte : bytes) byte = random();
        pxadb::decode_bulk(bytes.data(), bytes.size(), random() % 513);
        pxadb::parse_fast(bytes.data(), bytes.size(), random()%2049,&sequence,&offset,&length);
    }
}
