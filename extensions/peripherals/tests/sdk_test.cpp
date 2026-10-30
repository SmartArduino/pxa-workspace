// SPDX-License-Identifier: Apache-2.0
#include <pxa_control_capture.h>
#include <pxa-ext/peripherals.hpp>
#include <cassert>
#include <cstdio>
namespace ports = pxa::ext::peripherals;
static pxa::Context* context;
static std::uint64_t token;
static std::uint16_t service, opcode;
static unsigned closes;
static bool done;
static uint16_t selected_minor = PXA_PERIPHERALS_MINOR;
static pxa::Permission permission;
static ports::Port port;
static pxa::HapticsActuator motor;
extern "C" std::int32_t pxa_test_control(const std::uint8_t* data, std::uint32_t size) {
    const auto* p = reinterpret_cast<const std::byte*>(data);
    service = pxa::wire::get16(p); opcode = pxa::wire::get16(p+2); token = pxa::wire::get64(p+4);
    if (service == 1) { if (opcode == 2) ++closes; return 0; }
    if (service == 23 && opcode == 3) {
        assert(size == 36 && pxa::wire::get32(p+28) == 100 && pxa::wire::get16(p+32) == 1000);
    }
    if (service == ports::service_id && opcode == 5) {
        assert(size == 33 && p[32] == std::byte{42});
    }
    if (service == ports::service_id && opcode == 1)
        assert(size == 44 && pxa::wire::get16(p + 38) == 1 && pxa::wire::get16(p + 42) == PXA_PERIPHERALS_MINOR);
    return 0;
}
extern "C" std::int32_t pxa_io(std::uint64_t handle, std::uint32_t op, std::uint8_t* data, std::uint32_t size) {
    assert(handle == UINT64_C(0x100000006));
    if (size == 3) { assert(op == 2 && data[0] == 7 && data[1] == 17 && data[2] == 255); return 3; }
    assert(size == 1);
    if (op == 1) data[0] = 1; else assert(op == 2 && data[0] == 1);
    return 1;
}
static pxa::Task<void> run() {
    auto client = co_await ports::open(*context); assert(client && client->version_minor() == selected_minor);
    std::array<ports::PortInfo, 2> listed;
    auto count = co_await client->list(listed);
    assert(count && *count == 1 && listed[0].scope() == "expansion.i2c");
    auto opened = co_await client->open_port(listed[0], 3, 100000, 0x20, permission);
    assert(opened); port = std::move(*opened);
    std::array<std::byte, 1> tx{std::byte{42}};
    auto transfer = port.transfer(tx, 2); tx[0] = std::byte{0}; // lazy task owns its request bytes
    auto received = co_await std::move(transfer);
    assert(received && received->size == 2 && received->bytes[1] == std::byte{20});
    std::array<std::byte, 1> value{};
    assert(port.read(value) && value[0] == std::byte{1}); assert(port.write(value));
    auto color = port.rgb(7, 17, 255); assert(color && *color == 3);
    assert(port.close());
    auto caps = co_await context->haptics().capabilities(); assert(caps && caps->supported() && !caps->amplitude_control());
    auto actuator = co_await context->haptics().open(permission); assert(actuator); motor = std::move(*actuator);
    assert(co_await motor.vibrate(100)); assert(co_await motor.stop()); assert(motor.close());
    done = true; co_return pxa::Result<void>{};
}
struct App {
    pxa::Result<void> on_start(pxa::Context& ctx) {
        context = &ctx; permission = pxa::Permission(ctx.transport(), UINT64_C(0x100000005));
        return ctx.foreground_tasks().start(run());
    }
    pxa::Result<bool> on_event(pxa::Context&, const pxa::Event&) { return true; }
    void on_stop(pxa::StopReason) { port.reset(); motor.reset(); permission.reset(); }
};
PXA_APPLICATION(App)
static void answer(std::span<const std::byte> body) {
    std::array<std::byte, 1600> packet{};
    pxa::wire::put16(packet.data(), service); pxa::wire::put16(packet.data()+2, opcode);
    pxa::wire::put64(packet.data()+4, token); pxa::wire::put32(packet.data()+12, body.size()+4);
    std::copy(body.begin(), body.end(), packet.begin()+24);
    assert(pxa_app_on_event(reinterpret_cast<const std::uint8_t*>(packet.data()), 24+body.size()) >= 0);
}
int main(int argc, char**) {
    if (argc > 1) selected_minor = 1;
    assert(pxa_app_start(nullptr, 0) == 0);
    std::array<std::byte, 20> hello{}; const std::uint8_t identity[] = PXA_PERIPHERALS_IDENTITY;
    std::memcpy(hello.data(), identity, 16); pxa::wire::put16(hello.data()+18, selected_minor);
    assert(opcode == 1); answer(hello);
    std::array<std::byte, 25> list{};
    pxa::wire::put16(list.data(), 1); pxa::wire::put16(list.data()+2, 3); pxa::wire::put16(list.data()+4, 2);
    pxa::wire::put32(list.data()+6, 3); pxa::wire::put16(list.data()+10, 13);
    std::memcpy(list.data()+12, "expansion.i2c", 13);
    assert(opcode == 2); answer(list);
    std::array<std::byte, 8> handle{}; pxa::wire::put64(handle.data(), UINT64_C(0x100000006));
    assert(opcode == 4); answer(handle);
    std::array<std::byte, 2> data{std::byte{10}, std::byte{20}};
    assert(opcode == 5); answer(data);
    std::array<std::byte, 8> caps{}; pxa::wire::put32(caps.data(), 1); pxa::wire::put32(caps.data()+4, 1000);
    assert(service == 23 && opcode == 1); answer(caps);
    assert(opcode == 2); answer(handle);
    assert(opcode == 3); answer({}); assert(opcode == 4); answer({});
    assert(done && closes == 2);
    pxa_app_stop(0);
    puts("Peripherals and Haptics SDK: negotiation, resource ownership, request copies and wire encoding passed");
}
