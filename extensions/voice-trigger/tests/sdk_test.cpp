// SPDX-License-Identifier: Apache-2.0
#include <pxa_control_capture.h>
#include <pxa-ext/voice_trigger.hpp>
#include <cassert>
#include <cstdio>

namespace vt = pxa::ext::voice_trigger;
static pxa::Context *context;
static std::uint64_t request_token;
static std::uint16_t request_opcode;
static std::optional<pxa::Error> probe_error;
static std::optional<vt::Client> client;
static std::array<vt::Profile, 1> profiles;
static std::array<vt::Keyword, 16> words;
static vt::Subscription subscription;
static pxa::Permission permission;
static unsigned closes, cancels, triggers;
static bool unsupported, done;

extern "C" std::int32_t pxa_test_control(const std::uint8_t *data, std::uint32_t size) {
    const auto p = reinterpret_cast<const std::byte *>(data);
    const auto sid = pxa::wire::get16(p);
    if (sid == 1) {
        if (pxa::wire::get16(p + 2) == 1) ++cancels;
        else { assert(pxa::wire::get16(p + 2) == 2 && size == 28); ++closes; }
        return 0;
    }
    assert(sid == vt::service_id);
    request_token = pxa::wire::get64(p + 4);
    request_opcode = pxa::wire::get16(p + 2);
    if (request_opcode == PXA_VT_SUBSCRIBE) {
        assert(size == 36 && pxa::wire::get32(p + 20) == 1);
        assert(pxa::wire::get32(p + 24) == 7);
        assert(pxa::wire::get64(p + 28) == UINT64_C(0x100000005));
    }
    return unsupported ? -3 : 0;
}
extern "C" std::int32_t pxa_io(std::uint64_t, std::uint32_t, std::uint8_t *, std::uint32_t) {
    assert(false); return -3;
}

static pxa::Task<void> probe() {
    auto opened = co_await vt::open(*context);
    if (opened) { client = *opened; probe_error.reset(); }
    else probe_error = opened.error();
    done = true;
    co_return pxa::Result<void>{};
}
static pxa::Task<void> setup() {
    auto caps = co_await client->capabilities();
    assert(caps && caps->ready && !caps->playback_detection());
    auto listed = co_await client->list_profiles(profiles);
    assert(listed && *listed == 1 && profiles[0].key.view() == vt::default_scope);
    auto keywords = co_await client->list_keywords(profiles[0], words);
    assert(keywords && *keywords == 1 && words[0].label.view() == "你好小智");
    auto opened = co_await client->subscribe(profiles[0], permission);
    assert(opened);
    subscription = std::move(*opened);
    done = true;
    co_return pxa::Result<void>{};
}
static pxa::Task<void> subscribe_late() {
    auto opened = co_await client->subscribe(profiles[0], permission);
    assert(opened);
    subscription = std::move(*opened);
    co_return pxa::Result<void>{};
}
struct TestApp {
    pxa::Result<void> on_start(pxa::Context &ctx) {
        context = &ctx;
        return ctx.foreground_tasks().start(probe());
    }
    pxa::Result<bool> on_event(pxa::Context &, const pxa::Event &event) {
        auto result = subscription.notification(event);
        if (result && !result->failure) ++triggers;
        return true;
    }
    void on_background(pxa::Context &) { subscription.reset(); }
    void on_stop(pxa::StopReason) { subscription.reset(); permission.reset(); }
};
PXA_APPLICATION(TestApp)

static void deliver(std::uint16_t opcode, std::uint64_t token,
    std::span<const std::byte> payload) {
    std::array<std::byte, 2048> packet{};
    pxa::wire::put16(packet.data(), vt::service_id);
    pxa::wire::put16(packet.data() + 2, opcode);
    pxa::wire::put64(packet.data() + 4, token);
    pxa::wire::put32(packet.data() + 12, static_cast<std::uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), packet.begin() + 20);
    assert(pxa_app_on_event(reinterpret_cast<const std::uint8_t *>(packet.data()),
        20 + payload.size()) >= 0);
}
static void answer(std::span<const std::byte> body) {
    std::array<std::byte, 1200> result{};
    std::copy(body.begin(), body.end(), result.begin() + 4);
    deliver(request_opcode, request_token, std::span(result).first(4 + body.size()));
}
static void begin(pxa::Task<void> task) {
    context->transport().phase(pxa::Phase::event);
    done = false;
    assert(context->foreground_tasks().start(std::move(task)));
}

int main() {
    unsupported = true;
    assert(pxa_app_start(nullptr, 0) == 0 && done && probe_error == pxa::Error::unsupported);
    unsupported = false;
    begin(probe());
    std::array<std::byte, 20> hello{};
    constexpr std::uint8_t identity[] = PXA_VT_IDENTITY;
    std::memcpy(hello.data(), identity, 16);
    pxa::wire::put16(hello.data() + 18, 1);
    hello[0] ^= std::byte{1};
    answer(hello);
    assert(done && probe_error == pxa::Error::protocol_error);
    begin(probe()); hello[0] ^= std::byte{1}; answer(hello);
    assert(done && client && !probe_error);
    permission = pxa::Permission(context->transport(), UINT64_C(0x100000005));
    begin(setup());
    std::array<std::byte, 24> caps{};
    pxa::wire::put32(caps.data(), PXA_VT_FOREGROUND_ONLY);
    pxa::wire::put32(caps.data() + 4, 1);
    pxa::wire::put16(caps.data() + 8, 1); pxa::wire::put16(caps.data() + 10, 1);
    pxa::wire::put16(caps.data() + 12, 16); pxa::wire::put16(caps.data() + 14, 63);
    pxa::wire::put32(caps.data() + 16, 7);
    assert(request_opcode == PXA_VT_CAPABILITIES); answer(caps);
    std::array<std::byte, 28> profile{};
    pxa::wire::put16(profile.data(), 1); pxa::wire::put32(profile.data() + 2, 1);
    pxa::wire::put32(profile.data() + 6, 7); pxa::wire::put16(profile.data() + 10, 1);
    pxa::wire::put16(profile.data() + 12, 14);
    std::memcpy(profile.data() + 14, "system.default", 14);
    assert(request_opcode == PXA_VT_LIST_PROFILES); answer(profile);
    std::array<std::byte, 26> keyword{};
    pxa::wire::put32(keyword.data(), 7); pxa::wire::put16(keyword.data() + 4, 1);
    pxa::wire::put32(keyword.data() + 8, 42); pxa::wire::put16(keyword.data() + 12, 12);
    std::memcpy(keyword.data() + 14, "你好小智", 12);
    assert(request_opcode == PXA_VT_LIST_KEYWORDS); answer(keyword);
    std::array<std::byte, 12> handle{};
    pxa::wire::put64(handle.data(), UINT64_C(0x100000006)); pxa::wire::put32(handle.data() + 8, 7);
    assert(request_opcode == PXA_VT_SUBSCRIBE); answer(handle);
    assert(done && subscription);
    std::array<std::byte, 40> event{};
    pxa::wire::put64(event.data(), subscription.handle());
    pxa::wire::put64(event.data() + 8, 1); pxa::wire::put64(event.data() + 16, 1000);
    pxa::wire::put32(event.data() + 24, 7); pxa::wire::put32(event.data() + 28, 42);
    pxa::wire::put32(event.data() + 32, PXA_VT_TRIGGERED);
    deliver(PXA_VT_EVENT, 0, event); assert(triggers == 1);
    deliver(PXA_VT_EVENT, 0, event); assert(triggers == 1); // Retransmission.
    pxa::wire::put64(event.data(), UINT64_C(0x200000006));
    deliver(PXA_VT_EVENT, 0, event); assert(triggers == 1); // Stale generation.
    assert(!vt::decode_keywords(std::span(keyword).first(25), profiles[0], words));
    keyword[14] = std::byte{0xff};
    assert(!vt::decode_keywords(keyword, profiles[0], words));
    // Resource-returning Context requests must close a late successful handle.
    context->transport().phase(pxa::Phase::event);
    subscription.reset();
    begin(subscribe_late());
    const auto close_before = closes;
    context->foreground_tasks().cancel(); assert(cancels == 1);
    pxa::wire::put64(handle.data(), UINT64_C(0x300000006));
    answer(handle); assert(closes == close_before + 1);
    pxa_app_stop(0);
    std::puts("VoiceTrigger C++ SDK integration OK");
}
