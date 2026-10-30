// SPDX-License-Identifier: Apache-2.0
#include <pxa-ext/vision.hpp>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace pxa::ext::peripherals;
struct Stream {
    std::vector<std::byte> data;
    size_t offset = 0, chunk = 3;
    bool blocked = false, bad_count = false;
    pxa::Result<uint32_t> read(std::span<std::byte> output) {
        if (bad_count) return output.size() + 1;
        blocked = !blocked;
        if (blocked) return std::unexpected(pxa::Error::would_block);
        size_t n = std::min({chunk, output.size(), data.size() - offset});
        std::copy_n(data.begin() + offset, n, output.begin()); offset += n; return n;
    }
};
int main() {
    Stream stream; stream.data.resize(PXA_VISION_HEADER_BYTES + 600);
    const FrameInfo expected{PXA_VISION_JPEG, 600, 0x98765432, UINT64_C(0xfedcba9876543210)};
    assert(pxa_vision_encode(reinterpret_cast<uint8_t*>(stream.data.data()), &expected));
    for (size_t i = 24; i < stream.data.size(); ++i) stream.data[i] = std::byte(i);
    FrameReader reader(PXA_VISION_JPEG);
    assert(!reader.next());
    std::array<std::byte, 700> payload{};
    assert(!reader.read_payload(stream, payload));
    pxa::Result<FrameInfo> header = std::unexpected(pxa::Error::would_block);
    for (unsigned i = 0; i < 100 && !header; ++i) {
        header = reader.header(stream);
        if (!header) assert(header.error() == pxa::Error::would_block);
    }
    assert(header && header->sequence == expected.sequence && header->monotonic_us == expected.monotonic_us);
    stream.chunk = 256;
    size_t used = 0;
    while (!reader.complete()) {
        auto n = reader.read_payload(stream, std::span(payload).subspan(used));
        if (n) used += *n; else assert(n.error() == pxa::Error::would_block);
    }
    assert(used == 600 && stream.offset == stream.data.size());
    assert(std::equal(payload.begin(), payload.begin() + used, stream.data.begin() + 24));
    assert(reader.read_payload(stream, payload).value() == 0);
    assert(reader.next() && !reader.complete());
    for (unsigned corrupt : {0u, 4u, 6u, 8u}) {
        Stream bad; bad.chunk = 24; bad.data.assign(stream.data.begin(), stream.data.begin() + 24);
        bad.data[corrupt] = std::byte{0};
        if (corrupt == 8) { bad.data[9] = std::byte{0}; bad.data[10] = std::byte{5}; }
        FrameReader invalid(PXA_VISION_JPEG);
        assert(!invalid.header(bad));
        auto result = invalid.header(bad); assert(!result && result.error() == pxa::Error::protocol_error);
        assert(!invalid.read_payload(bad, payload));
    }
    Stream wrong_format; wrong_format.chunk = 24; wrong_format.data = stream.data;
    FrameReader json(PXA_VISION_JSON); assert(!json.header(wrong_format));
    assert(json.header(wrong_format).error() == pxa::Error::protocol_error);
    Stream count; count.bad_count = true;
    FrameReader bad_count(PXA_VISION_JPEG);
    assert(bad_count.header(count).error() == pxa::Error::protocol_error);
    FrameInfo info{};
    std::array<uint8_t, 24> bytes{};
    assert(!pxa_vision_encode(bytes.data(), &info));
    info = {PXA_VISION_JSON, PXA_VISION_MAX_JSON + 1, 0, 0};
    assert(!pxa_vision_encode(bytes.data(), &info));
    puts("Vision SDK: partial headers/payloads, WouldBlock, exact bounds, bad format/length/count, frame lifecycle passed");
}
