// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/app.hpp>
#include <pxa_ext/voice_trigger_protocol.h>

namespace pxa::ext::voice_trigger {

inline constexpr std::uint16_t service_id = PXA_VT_SERVICE_ID;
inline constexpr std::string_view permission_name = PXA_VT_PERMISSION;
inline constexpr std::string_view default_scope = PXA_VT_DEFAULT_SCOPE;
inline constexpr std::uint32_t default_profile = PXA_VT_DEFAULT_PROFILE;

struct Capabilities {
    std::uint32_t flags = 0, revision = 0, update_mode = 0;
    bool ready = false;
    std::uint16_t profiles = 0, max_subscriptions = 0,
                  max_keywords = 0, max_word_bytes = 0;
    bool playback_detection() const noexcept { return flags & PXA_VT_PLAYBACK_DETECTION; }
};
struct Profile {
    std::uint32_t id = 0, revision = 0;
    std::uint16_t keyword_count = 0;
    FixedText<32> key;
};
struct Keyword {
    std::uint32_t id = 0;
    FixedText<PXA_VT_MAX_WORD_BYTES> label;
};
struct Triggered {
    std::uint64_t subscription = 0, sequence = 0, timestamp_us = 0;
    std::uint32_t profile_revision = 0, keyword_id = 0;
};
struct Notification {
    Triggered trigger;
    std::optional<Error> failure;
};

inline Result<Capabilities> decode_capabilities(std::span<const std::byte> body) noexcept {
    if (body.size() != PXA_VT_CAPABILITIES_BYTES)
        return std::unexpected(Error::protocol_error);
    Capabilities c;
    c.flags = wire::get32(body.data());
    const auto ready = wire::get32(body.data() + 4);
    c.ready = ready == 1;
    c.profiles = wire::get16(body.data() + 8);
    c.max_subscriptions = wire::get16(body.data() + 10);
    c.max_keywords = wire::get16(body.data() + 12);
    c.max_word_bytes = wire::get16(body.data() + 14);
    c.revision = wire::get32(body.data() + 16);
    c.update_mode = wire::get32(body.data() + 20);
    if (ready > 1 || !(c.flags & PXA_VT_FOREGROUND_ONLY) ||
        (c.flags & ~(PXA_VT_FOREGROUND_ONLY | PXA_VT_PLAYBACK_DETECTION)) ||
        !c.profiles || c.profiles > 16 || !c.max_subscriptions ||
        !c.revision || !c.max_keywords || c.max_keywords > PXA_VT_MAX_KEYWORDS ||
        !c.max_word_bytes || c.max_word_bytes > PXA_VT_MAX_WORD_BYTES ||
        c.update_mode != PXA_VT_FIXED_CATALOG)
        return std::unexpected(Error::protocol_error);
    return c;
}

inline Result<std::size_t> decode_profiles(std::span<const std::byte> body,
    std::span<Profile> output) noexcept {
    if (body.size() < 2) return std::unexpected(Error::protocol_error);
    const auto count = wire::get16(body.data());
    if (!count || count > 16) return std::unexpected(Error::protocol_error);
    if (count > output.size()) return std::unexpected(Error::resource_limit);
    body = body.subspan(2);
    for (std::size_t i = 0; i < count; ++i) {
        if (body.size() < 12) return std::unexpected(Error::protocol_error);
        Profile p;
        p.id = wire::get32(body.data());
        p.revision = wire::get32(body.data() + 4);
        p.keyword_count = wire::get16(body.data() + 8);
        const auto size = wire::get16(body.data() + 10);
        if (!p.id || !p.revision || p.keyword_count > PXA_VT_MAX_KEYWORDS ||
            body.size() < 12u + size || !p.key.assign(body.subspan(12, size)) ||
            p.key.view().find('\0') != std::string_view::npos)
            return std::unexpected(Error::protocol_error);
        for (std::size_t j = 0; j < i; ++j)
            if (output[j].id == p.id || output[j].key.view() == p.key.view())
                return std::unexpected(Error::protocol_error);
        output[i] = p;
        body = body.subspan(12 + size);
    }
    if (!body.empty()) return std::unexpected(Error::protocol_error);
    return count;
}

inline Result<std::size_t> decode_keywords(std::span<const std::byte> body,
    const Profile& profile, std::span<Keyword> output) noexcept {
    if (body.size() < 8 || wire::get32(body.data()) != profile.revision ||
        wire::get16(body.data() + 6)) return std::unexpected(Error::protocol_error);
    const auto count = wire::get16(body.data() + 4);
    if (!count || count != profile.keyword_count || count > PXA_VT_MAX_KEYWORDS)
        return std::unexpected(Error::protocol_error);
    if (count > output.size()) return std::unexpected(Error::resource_limit);
    body = body.subspan(8);
    for (std::size_t i = 0; i < count; ++i) {
        if (body.size() < 6) return std::unexpected(Error::protocol_error);
        Keyword word;
        word.id = wire::get32(body.data());
        const auto size = wire::get16(body.data() + 4);
        if (!word.id || body.size() < 6u + size ||
            !word.label.assign(body.subspan(6, size)) ||
            word.label.view().find('\0') != std::string_view::npos)
            return std::unexpected(Error::protocol_error);
        for (std::size_t j = 0; j < i; ++j)
            if (output[j].id == word.id || output[j].label.view() == word.label.view())
                return std::unexpected(Error::protocol_error);
        output[i] = word;
        body = body.subspan(6 + size);
    }
    if (!body.empty()) return std::unexpected(Error::protocol_error);
    return count;
}

struct SubscriptionTag {};
class Subscription {
public:
    Subscription() = default;
    Subscription(Transport& transport, std::uint64_t handle, std::uint32_t revision) noexcept
        : resource_(transport, handle), revision_(revision) {}
    explicit operator bool() const noexcept { return bool(resource_); }
    std::uint64_t handle() const noexcept { return resource_.handle(); }
    Result<void> close() noexcept { return resource_.close(); }
    void reset() noexcept { resource_.reset(); }
    Result<Notification> notification(const Event& event) noexcept {
        if (event.service != service_id || event.opcode != PXA_VT_EVENT || event.token)
            return std::unexpected(Error::not_found);
        if (event.payload.size() != PXA_VT_EVENT_BYTES)
            return std::unexpected(Error::protocol_error);
        const auto p = event.payload.data();
        if (!resource_ || wire::get64(p) != handle())
            return std::unexpected(Error::not_found);
        Notification result{{wire::get64(p), wire::get64(p + 8), wire::get64(p + 16),
            wire::get32(p + 24), wire::get32(p + 28)}, {}};
        const auto kind = wire::get32(p + 32);
        const auto status = static_cast<std::int32_t>(wire::get32(p + 36));
        if (!result.trigger.timestamp_us || result.trigger.profile_revision != revision_)
            return std::unexpected(Error::protocol_error);
        if (kind == PXA_VT_FAILED) {
            if (status > -1 || status < -16 || result.trigger.keyword_id)
                return std::unexpected(Error::protocol_error);
            result.failure = static_cast<Error>(status);
        } else if (kind == PXA_VT_TRIGGERED) {
            if (status || !result.trigger.keyword_id || !result.trigger.sequence)
                return std::unexpected(Error::protocol_error);
            if (result.trigger.sequence <= last_sequence_)
                return std::unexpected(Error::not_found);
            if (result.trigger.timestamp_us < last_timestamp_)
                return std::unexpected(Error::protocol_error);
            last_sequence_ = result.trigger.sequence;
            last_timestamp_ = result.trigger.timestamp_us;
        } else return std::unexpected(Error::protocol_error);
        return result;
    }
private:
    Resource<SubscriptionTag> resource_;
    std::uint32_t revision_ = 0;
    std::uint64_t last_sequence_ = 0, last_timestamp_ = 0;
};

class Client {
public:
    explicit Client(Context& ctx) noexcept : ctx_(&ctx) {}
    Task<Capabilities> capabilities(this Client self) {
        auto response = co_await self.ctx_->request(service_id, PXA_VT_CAPABILITIES);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        co_return decode_capabilities(*body);
    }
    Task<std::size_t> list_profiles(this Client self, std::span<Profile> output) {
        auto response = co_await self.ctx_->request(service_id, PXA_VT_LIST_PROFILES);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        co_return decode_profiles(*body, output);
    }
    Task<std::size_t> list_keywords(const Profile& profile, std::span<Keyword> output) {
        if (!profile.id || !profile.revision)
            return Task<std::size_t>::failed(Error::invalid_argument);
        std::array<std::byte, 8> packet;
        wire::put32(packet.data(), profile.id);
        wire::put32(packet.data() + 4, profile.revision);
        return keywords_impl(*this, packet, profile, output);
    }
    Task<Subscription> subscribe(const Profile& profile, const Permission& permission) {
        if (!profile.id || !profile.revision || !(permission.handle() >> 32))
            return Task<Subscription>::failed(Error::invalid_argument);
        std::array<std::byte, 16> packet;
        wire::put32(packet.data(), profile.id);
        wire::put32(packet.data() + 4, profile.revision);
        wire::put64(packet.data() + 8, permission.handle());
        return subscribe_impl(*this, packet, profile.revision);
    }
private:
    static Task<std::size_t> keywords_impl(Client self, std::array<std::byte, 8> packet,
        Profile profile, std::span<Keyword> output) {
        auto response = co_await self.ctx_->request(service_id, PXA_VT_LIST_KEYWORDS, packet);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        co_return decode_keywords(*body, profile, output);
    }
    static Task<Subscription> subscribe_impl(Client self, std::array<std::byte, 16> packet,
        std::uint32_t revision) {
        auto response = co_await self.ctx_->request(service_id, PXA_VT_SUBSCRIBE, packet, true);
        if (!response) co_return std::unexpected(response.error());
        auto body = wire::result_body(response->payload);
        if (!body) co_return std::unexpected(body.error());
        auto pending = wire::result_resource<SubscriptionTag>(self.ctx_->transport(), *body);
        if (body->size() != 12 || !(pending.handle() >> 32) ||
            wire::get32(body->data() + 8) != revision)
            co_return std::unexpected(Error::protocol_error);
        co_return Subscription(self.ctx_->transport(), pending.release(), revision);
    }
    Context *ctx_;
};

inline Task<Client> open(Context& ctx) {
    constexpr std::uint8_t identity[] = PXA_VT_IDENTITY;
    std::array<std::byte, 24> packet;
    std::memcpy(packet.data(), identity, 16);
    wire::put16(packet.data() + 16, PXA_VT_MAJOR);
    wire::put16(packet.data() + 18, PXA_VT_MINOR);
    wire::put16(packet.data() + 20, PXA_VT_MAJOR);
    wire::put16(packet.data() + 22, PXA_VT_MINOR);
    auto response = co_await ctx.request(service_id, PXA_VT_HELLO, packet);
    if (!response) co_return std::unexpected(response.error());
    auto body = wire::result_body(response->payload);
    if (!body) co_return std::unexpected(body.error());
    if (body->size() != 20 || std::memcmp(body->data(), identity, 16) ||
        wire::get16(body->data() + 16) != PXA_VT_MAJOR ||
        wire::get16(body->data() + 18) != PXA_VT_MINOR)
        co_return std::unexpected(Error::protocol_error);
    co_return Client(ctx);
}
} // namespace pxa::ext::voice_trigger
