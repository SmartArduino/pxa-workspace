#!/usr/bin/env python3
"""Test the queue and actual board reporting method without touching a device."""
import argparse
from pathlib import Path
import subprocess
import tempfile

BOARD = Path(__file__).resolve().parents[1]

# Extract the real method so a board regression (e.g. consuming on failure) is
# exercised alongside the queue. The shim models the nonblocking Host boundary.
SHIM = r"""
#include "doit_gamekit_controller.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using doit_gamekit::ControllerSamples;
static bool locked;
#define portENTER_CRITICAL(lock) do { (void)(lock); assert(!locked); locked=true; } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); assert(locked); locked=false; } while (0)
struct Report { bool connected; uint32_t buttons; };
static std::vector<Report> delivered;
static unsigned busy;
static void (*during_post)();
bool pxa_host_post_controller_state(uint8_t id, bool connected, uint32_t buttons) {
    assert(id == 0 && !locked);
    if (busy) { --busy; return false; }
    delivered.push_back({connected, buttons});
    if (during_post) { auto action = during_post; during_post = nullptr; action(); }
    return true;
}
class Gamekit {
public:
    ControllerSamples<> remote_controller_;
    int controller_lock_ = 0;
    bool reported_connected_ = true;
    bool gamepad_consumed_ = false;
    bool swallow_until_release_ = false, pointer_mode_ = false, focus_mode_ = false;
    uint32_t report_tick_ = 0, reported_buttons_ = 0;
    void ReportController(uint32_t report, uint32_t now);
};
"""

CASES = r"""
static Gamekit board;
int main() {
    board.ReportController(0, 5);
    assert(delivered.empty()); // no new periodic traffic while idle
    assert(board.remote_controller_.Push(true, 1));
    assert(board.remote_controller_.Push(true, 0));
    assert(board.remote_controller_.Push(true, 2));
    assert(board.remote_controller_.Push(true, 0));
    busy = 3;
    for (uint32_t now = 10; now < 25; now += 5) board.ReportController(0, now);
    assert(delivered.empty() && board.remote_controller_.Peek().buttons() == 1);
    for (uint32_t now = 25; now <= 40; now += 5) board.ReportController(0, now);
    assert(delivered.size() == 4);
    const uint32_t expected[] = {1, 0, 2, 0};
    for (unsigned i = 0; i < 4; ++i)
        assert(delivered[i].connected && delivered[i].buttons == expected[i]);
    for (uint32_t now = 45; now < 90; now += 5) board.ReportController(0, now);
    assert(delivered.size() == 4);
    board.ReportController(0, 90);
    assert(delivered.size() == 5 && delivered.back().buttons == 0);
    assert(board.remote_controller_.Push(false, 0));
    board.ReportController(0, 95);
    assert(!delivered.back().connected);
    board.remote_controller_.Clear();
    board.ReportController(4, 100);
    assert(delivered.back().connected && delivered.back().buttons == 4);

    // Cancellation/takeover racing an accepted post cannot acknowledge a
    // replacement queue, including a replacement with the identical state.
    assert(board.remote_controller_.Push(true, 2));
    during_post = [] {
        board.remote_controller_.Clear();
        assert(board.remote_controller_.Push(true, 2));
        assert(board.remote_controller_.Push(true, 0));
    };
    board.ReportController(0, 105);
    assert(board.remote_controller_.Peek().pending);
    assert(board.remote_controller_.Peek().buttons() == 2);
    board.ReportController(0, 110);
    assert(board.remote_controller_.Peek().buttons() == 0);
    board.ReportController(0, 115);
    assert(!board.remote_controller_.Peek().pending);

    // Unlocking consumes physical and remote buttons alike. Suppressed queued
    // edges still commit, otherwise a press could reappear after unlocking.
    board.remote_controller_.Clear();
    board.gamepad_consumed_ = true;
    assert(board.remote_controller_.Push(true, 16));
    assert(board.remote_controller_.Push(true, 0));
    board.ReportController(32, 120);
    assert(delivered.back().buttons == 0);
    assert(board.remote_controller_.Peek().buttons() == 0);
    board.ReportController(32, 125);
    assert(delivered.back().buttons == 0 && !board.remote_controller_.Peek().pending);
    board.gamepad_consumed_ = false;
    assert(board.remote_controller_.Push(true, 32));
    board.ReportController(0, 130);
    assert(delivered.back().buttons == 32);
    board.remote_controller_.Clear();
    board.ReportController(0, 135);

    // Heartbeat arithmetic remains valid across the 32-bit LVGL tick wrap.
    board.report_tick_ = UINT32_MAX - 10;
    const auto before = delivered.size();
    board.ReportController(0, 38);
    assert(delivered.size() == before);
    board.ReportController(0, 39);
    assert(delivered.size() == before + 1);
    assert(!locked);
    std::puts("actual board reporting: ordered edges, busy retries, cancellation and tick wrap passed");
}
"""

ACK_SHIM = r"""
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <string>
struct InputCommand { unsigned request_sequence, event_sequence; };
static uint64_t timer_us = 2000000;
static uint32_t log_ms = 3600;
static std::string reply;
uint64_t esp_timer_get_time() { return timer_us; }
uint32_t esp_log_timestamp() { return log_ms; }
void SendFrame(unsigned sequence, const char* kind, const char* text) {
    assert(sequence == 42 && std::string(kind) == "OK");
    reply = text;
}
"""
ACK_CASES = r"""
int main() {
    SendInputAck({0, 1}, "processed");
    assert(reply.empty());
    SendInputAck({42, 1}, "processed");
    assert(reply == "event=1;device_us=2000000;log_ms=3600;status=processed");
    timer_us = INT64_MAX; log_ms = UINT32_MAX;
    SendInputAck({42, UINT32_MAX}, "processed");
    assert(reply == "event=4294967295;device_us=9223372036854775807;log_ms=4294967295;status=processed");
    assert(reply.size() < 112);
    std::puts("actual input ACK: distinct timer/log clocks, no-ACK requests and field limits passed");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source = (BOARD / "src/doit_gamekit_board.cc").read_text()
    begin = source.index("void Gamekit::ReportController(")
    end = source.index("\nbool Gamekit::ConfigureDiagnostics()", begin)
    service = (BOARD.parents[1] / "components/pxadb/src_pxa_service.cc").read_text()
    ack_begin = service.index("void SendInputAck(")
    ack_end = service.index("\nbool QueueInput(", ack_begin)
    with tempfile.TemporaryDirectory(prefix="gamekit-controller-") as tmp:
        tmp = Path(tmp)
        integration = tmp / "board_test.cc"
        integration.write_text(SHIM + source[begin:end] + CASES)
        ack = tmp / "ack_test.cc"
        ack.write_text(ACK_SHIM + service[ack_begin:ack_end] + ACK_CASES)
        for test in [BOARD / "tests/controller_test.cc", integration, ack]:
            binary = tmp / test.stem
            subprocess.run([
                args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(BOARD / "include"), str(test), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
