// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "peripherals.hpp"
#include <pxa_ext/vision_frame.h>
namespace pxa::ext::peripherals {
using FrameInfo = pxa_vision_frame_info_t;
// One reader per STREAM port. No heap or image decoder. Poll header(), then
// drain read_payload() into caller-owned storage. WOULD_BLOCK preserves all
// progress. A malformed stream requires closing its port, not scanning bytes
// for another magic. Start the next frame only after complete().
class FrameReader {
public:
    explicit FrameReader(std::uint16_t expected_format) noexcept : expected_(expected_format) {}
    template<class Stream> Result<FrameInfo> header(Stream& port) noexcept {
        if (failed_) return std::unexpected(Error::protocol_error);
        if (header_used_ == header_.size()) return info_;
        auto part = port.read(std::span(header_).subspan(header_used_));
        if (!part) return std::unexpected(part.error());
        if (*part > header_.size() - header_used_) return fail<FrameInfo>();
        header_used_ += *part;
        if (header_used_ < header_.size()) return std::unexpected(Error::would_block);
        if (!pxa_vision_decode(reinterpret_cast<const std::uint8_t*>(header_.data()), &info_) ||
            info_.format != expected_) return fail<FrameInfo>();
        return info_;
    }
    template<class Stream> Result<std::size_t> read_payload(Stream& port, std::span<std::byte> output) noexcept {
        if (failed_) return std::unexpected(Error::protocol_error);
        if (header_used_ != header_.size() || output.empty()) return std::unexpected(Error::invalid_argument);
        if (complete()) return std::size_t{0};
        const auto size = std::min({output.size(), std::size_t(info_.size - payload_used_), std::size_t(256)});
        auto part = port.read(output.first(size));
        if (!part) return std::unexpected(part.error());
        if (*part > size) return fail<std::size_t>();
        payload_used_ += *part;
        return *part;
    }
    bool complete() const noexcept { return !failed_ && info_.size && payload_used_ == info_.size; }
    Result<void> next() noexcept {
        if (!complete()) return std::unexpected(Error::invalid_argument);
        header_used_ = payload_used_ = 0; info_ = {};
        return {};
    }
private:
    template<class T> Result<T> fail() noexcept { failed_ = true; return std::unexpected(Error::protocol_error); }
    std::array<std::byte, PXA_VISION_HEADER_BYTES> header_{};
    std::size_t header_used_ = 0, payload_used_ = 0;
    FrameInfo info_{};
    std::uint16_t expected_;
    bool failed_ = false;
};
}
