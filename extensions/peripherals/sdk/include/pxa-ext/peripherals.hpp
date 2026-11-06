// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/app.hpp>
#include <pxa_ext/peripherals_protocol.h>
#include <array>
#include <cstring>
namespace pxa::ext::peripherals {
inline constexpr std::uint16_t service_id = PXA_PERIPHERALS_SERVICE_ID;
struct PortInfo {
    std::uint16_t id = 0, kind = 0;
    std::uint32_t flags = 0;
    std::array<char, 32> name{};
    std::string_view scope() const noexcept {
        const auto end = std::find(name.begin(), name.end(), '\0');
        return {name.data(), std::size_t(end-name.begin())};
    }
    bool available() const noexcept { return !(flags & PXA_PERIPHERALS_RESERVED); }
};
struct ModuleInfo {
    std::uint32_t generation = 0, flags = 0;
    std::uint16_t board_type = 0, board_id = 0, hardware_version = 0, vendor_id = 0;
    std::uint32_t serial_number = 0;
    std::array<char, 32> name{};
    bool present() const noexcept { return flags & 1; }
    bool valid() const noexcept { return flags & 2; }
    bool claimed() const noexcept { return flags & 4; }
};
struct CameraProfile {
    std::uint32_t id = 0;
    std::uint16_t encoding = 0, width = 0, height = 0, quality = 0;
    std::uint32_t max_frame_bytes = 0, period_us = 0, flags = 0;
    bool is_default() const noexcept { return flags & PXA_CAMERA_PROFILE_DEFAULT; }
    bool native_size() const noexcept { return flags & PXA_CAMERA_PROFILE_NATIVE_SIZE; }
};
struct Received {
    std::array<std::byte, PXA_PERIPHERALS_MAX_TRANSFER> bytes{};
    std::size_t size = 0;
    std::span<const std::byte> data() const noexcept { return {bytes.data(), size}; }
};
struct FrameStreamTag {};
// One finite JPEG payload. It can be consumed by the Host image decoder or
// read into app memory when a photograph is saved. Closing an incomplete
// frame closes its camera port, so later reads cannot start mid-frame.
class FrameStream {
public:
    FrameStream(Context& ctx, std::uint64_t handle) noexcept
        : resource_(ctx.transport(), handle), context_(&ctx) {}
    Result<void> close() noexcept { return resource_.close(); }
    ReadableResource readable() const & noexcept {
        return {&context_->transport(), resource_.handle(), 2};
    }
    Result<std::uint32_t> read(std::span<std::byte> output) noexcept {
        return context_->transport().io(resource_.handle(), 1, output);
    }
private:
    Resource<FrameStreamTag> resource_;
    Context* context_;
};
struct PortTag {};
class Port {
    friend class Client;
public:
    Port() = default;
    Port(Context& ctx, std::uint64_t handle) noexcept : resource_(ctx.transport(), handle), ctx_(&ctx) {}
    std::uint64_t handle() const noexcept { return resource_.handle(); }
    explicit operator bool() const noexcept { return bool(resource_); }
    Result<void> close() noexcept { return resource_.close(); }
    void reset() noexcept { resource_.reset(); }
    Result<std::uint32_t> read(std::span<std::byte> output) noexcept {
        if (!resource_ || output.empty() || output.size() > 256) return std::unexpected(Error::invalid_argument);
        return ctx_->transport().io(handle(), 1, output);
    }
    Result<std::uint32_t> write(std::span<const std::byte> input) noexcept {
        if (!resource_ || input.empty() || input.size() > 256) return std::unexpected(Error::invalid_argument);
        std::array<std::byte, 256> copy;
        std::copy(input.begin(), input.end(), copy.begin());
        return ctx_->transport().io(handle(), 2, std::span(copy).first(input.size()));
    }
    Result<std::uint32_t> rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept {
        const std::array color{std::byte(red), std::byte(green), std::byte(blue)};
        return write(color);
    }
    Task<Received> transfer(std::span<const std::byte> tx, std::uint16_t rx_size) {
        if (!resource_ || tx.size() > 256 || rx_size > 256 || (tx.empty() && !rx_size))
            return Task<Received>::failed(Error::invalid_argument);
        wire::RequestPacket<268> packet;
        wire::put64(packet.bytes.data(), handle());
        wire::put16(packet.bytes.data()+8, tx.size()); wire::put16(packet.bytes.data()+10, rx_size);
        std::copy(tx.begin(), tx.end(), packet.bytes.begin()+12); packet.size = 12 + tx.size();
        return transfer_impl(*ctx_, packet, rx_size);
    }
private:
    static Task<Received> transfer_impl(Context& ctx, wire::RequestPacket<268> packet, std::uint16_t size) {
        auto response = co_await ctx.request(service_id, PXA_PERIPHERALS_TRANSFER, packet.packet());
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        if (body->size() != size) co_return std::unexpected(Error::protocol_error);
        Received result; result.size = size; std::copy(body->begin(), body->end(), result.bytes.begin());
        co_return result;
    }
    Resource<PortTag> resource_;
    Context* ctx_ = nullptr;
};
class Client {
public:
    explicit Client(Context& ctx, std::uint16_t minor = PXA_PERIPHERALS_MINOR) noexcept : ctx_(&ctx), minor_(minor) {}
    std::uint16_t version_minor() const noexcept { return minor_; }
    Task<Asset> preview(Port& port) {
        if (minor_ < 7) return Task<Asset>::failed(Error::unsupported);
        if (!port || port.ctx_ != ctx_) return Task<Asset>::failed(Error::invalid_argument);
        std::array<std::byte,8> packet{};
        wire::put64(packet.data(),port.handle());
        return preview_impl(*this,packet);
    }
    Task<std::size_t> camera_profiles(this Client self, const PortInfo& port,
                                     std::span<CameraProfile> output) {
        if (self.minor_ < 5) co_return std::unexpected(Error::unsupported);
        if (port.kind != PXA_PERIPHERALS_VISION || port.scope() != "camera.jpeg")
            co_return std::unexpected(Error::invalid_argument);
        wire::RequestPacket<2> packet;
        wire::put16(packet.bytes.data(), port.id); packet.size = 2;
        auto response = co_await self.ctx_->request(service_id, PXA_PERIPHERALS_CAMERA_PROFILES, packet.packet());
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        if (body->size() < 2) co_return std::unexpected(Error::protocol_error);
        const auto count = wire::get16(body->data());
        if (!count || count > PXA_CAMERA_MAX_PROFILES || body->size() != 2u + 24u * count)
            co_return std::unexpected(Error::protocol_error);
        if (count > output.size()) co_return std::unexpected(Error::resource_limit);
        unsigned defaults = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto* p = body->data() + 2 + i * 24;
            CameraProfile value{wire::get32(p), wire::get16(p+4), wire::get16(p+6),
                wire::get16(p+8), wire::get16(p+10), wire::get32(p+12), wire::get32(p+16), wire::get32(p+20)};
            if (!value.id || value.encoding != PXA_VISION_JPEG || value.quality > 100 ||
                (!value.width != !value.height) || (!value.width && !value.native_size()) ||
                !value.max_frame_bytes || value.max_frame_bytes > PXA_VISION_MAX_JPEG || (value.flags & ~3u))
                co_return std::unexpected(Error::protocol_error);
            for (unsigned j = 0; j < i; ++j) if (output[j].id == value.id)
                co_return std::unexpected(Error::protocol_error);
            defaults += value.is_default(); output[i] = value;
        }
        if (defaults != 1) co_return std::unexpected(Error::protocol_error);
        co_return std::size_t(count);
    }
    Task<std::size_t> list(this Client self, std::span<PortInfo> output) {
        auto response = co_await self.ctx_->request(service_id, PXA_PERIPHERALS_LIST);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        if (body->size() < 2) co_return std::unexpected(Error::protocol_error);
        const auto count = wire::get16(body->data());
        if (count > PXA_PERIPHERALS_MAX_PORTS) co_return std::unexpected(Error::protocol_error);
        if (count > output.size()) co_return std::unexpected(Error::resource_limit);
        std::size_t offset = 2;
        for (std::size_t i = 0; i < count; ++i) {
            if (body->size()-offset < 10) co_return std::unexpected(Error::protocol_error);
            const auto* data = body->data()+offset; auto& port = output[i]; port = {};
            port.id = wire::get16(data); port.kind = wire::get16(data+2); port.flags = wire::get32(data+4);
            const auto length = wire::get16(data+8); offset += 10;
            if (!port.id || port.kind < 1 || port.kind > (self.minor_ >= 3 ? PXA_PERIPHERALS_VISION : self.minor_ >= 2 ? PXA_PERIPHERALS_RGB : PXA_PERIPHERALS_SLOT) || (port.flags & ~7u) || !length ||
                length >= 32 || body->size()-offset < length) co_return std::unexpected(Error::protocol_error);
            for (std::size_t j = 0; j < length; ++j) {
                const auto c = std::to_integer<unsigned>(body->data()[offset+j]);
                if (c < 0x21 || c > 0x7e) co_return std::unexpected(Error::protocol_error);
                port.name[j] = char(c);
            }
            for (std::size_t j = 0; j < i; ++j)
                if (output[j].id == port.id || output[j].scope() == port.scope())
                    co_return std::unexpected(Error::protocol_error);
            offset += length;
        }
        if (offset != body->size()) co_return std::unexpected(Error::protocol_error);
        co_return count;
    }
    Task<ModuleInfo> module(std::uint16_t port) {
        std::array<std::byte, 2> packet{}; wire::put16(packet.data(), port);
        return module_impl(*this, packet);
    }
    // parameter: output initial logical level, I2C Hz or UART baud; otherwise 0.
    Task<Port> open_port(const PortInfo& port, std::uint16_t mode, std::uint32_t parameter,
        std::uint16_t address, const Permission& permission) {
        if (!port.id || !port.available() || !(permission.handle() >> 32))
            return Task<Port>::failed(Error::invalid_argument);
        std::array<std::byte, 20> packet{};
        wire::put16(packet.data(), port.id); wire::put16(packet.data()+2, mode);
        wire::put32(packet.data()+4, parameter); wire::put16(packet.data()+8, address);
        wire::put64(packet.data()+12, permission.handle());
        return open_impl(*this, packet);
    }
    Task<FrameStream> frame(Port& port) {
        if (minor_ < 4) return Task<FrameStream>::failed(Error::unsupported);
        if (!port || port.ctx_ != ctx_) return Task<FrameStream>::failed(Error::invalid_argument);
        std::array<std::byte, 8> packet{};
        wire::put64(packet.data(), port.handle());
        return frame_impl(*this, packet, PXA_PERIPHERALS_FRAME_STREAM);
    }
    // next_cookie is the caller's next unused ReadAt cookie for this file.
    // Reserve one per possible short read. Gaps are valid in the FS protocol.
    Task<FrameStream> file_frame(File& file, std::uint64_t offset, std::uint32_t bytes,
                                 std::uint64_t& next_cookie) {
        if (minor_ < 6) return Task<FrameStream>::failed(Error::unsupported);
        if (!file || file.readable().transport != &ctx_->transport() || !bytes ||
            bytes > PXA_VISION_MAX_JPEG || offset > UINT64_MAX-bytes ||
            !next_cookie || next_cookie > UINT64_MAX-bytes)
            return Task<FrameStream>::failed(Error::invalid_argument);
        std::array<std::byte, 28> packet{};
        wire::put64(packet.data(), file.handle()); wire::put64(packet.data()+8, offset);
        wire::put32(packet.data()+16, bytes);
        wire::put64(packet.data()+20, next_cookie);
        next_cookie += bytes;
        return frame_impl(*this, packet, PXA_PERIPHERALS_JPEG_FILE_FRAME);
    }
private:
    static Task<Asset> preview_impl(Client self, std::array<std::byte,8> packet) {
        auto response=co_await self.ctx_->request(service_id,PXA_PERIPHERALS_CAMERA_PREVIEW,packet,true);
        if (!response) co_return std::unexpected(response.error());
        auto body=wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        auto pending=wire::result_resource<AssetTag>(self.ctx_->transport(),*body);
        if (body->size()!=28 || !(pending.handle()>>32)) co_return std::unexpected(Error::protocol_error);
        const auto* p=body->data()+8;
        AssetDescriptor descriptor{AssetKind(std::to_integer<uint8_t>(p[0])),std::to_integer<uint8_t>(p[1]),
            wire::get16(p+2),wire::get16(p+4),wire::get16(p+6),wire::get32(p+8),wire::get32(p+12),wire::get32(p+16)};
        if (descriptor.kind!=AssetKind::image || !descriptor.width || !descriptor.height ||
            descriptor.width>320 || descriptor.height>320 ||
            (descriptor.encoding!=2 && descriptor.encoding!=7 && descriptor.encoding!=8) ||
            descriptor.decoded_bytes!=uint32_t(descriptor.width)*descriptor.height*(descriptor.encoding==2 ? 2u : 4u) ||
            descriptor.resident_bytes<descriptor.decoded_bytes)
            co_return std::unexpected(Error::protocol_error);
        co_return Asset(self.ctx_->transport(),pending.release(),descriptor);
    }
    template<std::size_t N>
    static Task<FrameStream> frame_impl(Client self, std::array<std::byte, N> packet, std::uint16_t opcode) {
        auto response = co_await self.ctx_->request(service_id, opcode, packet, true);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        auto pending = wire::result_resource<FrameStreamTag>(self.ctx_->transport(), *body);
        if (body->size() != 8 || !(pending.handle() >> 32))
            co_return std::unexpected(Error::protocol_error);
        co_return FrameStream(*self.ctx_, pending.release());
    }
    static Task<ModuleInfo> module_impl(Client self, std::array<std::byte, 2> packet) {
        auto response = co_await self.ctx_->request(service_id, PXA_PERIPHERALS_MODULE_INFO, packet);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        if (body->size() != 52) co_return std::unexpected(Error::protocol_error);
        const auto* p = body->data(); ModuleInfo info;
        info.generation = wire::get32(p); info.flags = wire::get32(p+4);
        info.board_type = wire::get16(p+8); info.board_id = wire::get16(p+10);
        info.hardware_version = wire::get16(p+12); info.vendor_id = wire::get16(p+14);
        info.serial_number = wire::get32(p+16); std::memcpy(info.name.data(), p+20, 32);
        if ((info.flags & ~31u) || ((info.flags & 2u) && !(info.flags & 1u)) || info.name.back())
            co_return std::unexpected(Error::protocol_error);
        co_return info;
    }
    static Task<Port> open_impl(Client self, std::array<std::byte, 20> packet) {
        auto response = co_await self.ctx_->request(service_id, PXA_PERIPHERALS_OPEN, packet, true);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        auto pending = wire::result_resource<PortTag>(self.ctx_->transport(), *body);
        if (body->size() != 8 || !(pending.handle() >> 32)) co_return std::unexpected(Error::protocol_error);
        co_return Port(*self.ctx_, pending.release());
    }
    Context* ctx_;
    std::uint16_t minor_;
};
inline Task<Client> open(Context& ctx) {
    constexpr std::uint8_t identity[] = PXA_PERIPHERALS_IDENTITY;
    std::array<std::byte, 24> packet{}; std::memcpy(packet.data(), identity, 16);
    wire::put16(packet.data()+18, 1); wire::put16(packet.data()+22, PXA_PERIPHERALS_MINOR);
    auto response = co_await ctx.request(service_id, PXA_PERIPHERALS_HELLO, packet);
    if (!response) co_return std::unexpected(response.error());
    auto body = wire::result_body(response->payload);
    if (!body) co_return std::unexpected(body.error());
    if (body->size() != 20 || std::memcmp(body->data(), identity, 16) ||
        wire::get16(body->data()+16) || !wire::get16(body->data()+18) || wire::get16(body->data()+18) > PXA_PERIPHERALS_MINOR)
        co_return std::unexpected(Error::protocol_error);
    co_return Client(ctx, wire::get16(body->data()+18));
}
}
