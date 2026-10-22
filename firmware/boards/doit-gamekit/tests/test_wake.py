#!/usr/bin/env python3
"""Run both boards' actual wake/lock dispatch blocks against input sequences."""
from pathlib import Path
import subprocess
import tempfile

SHIM = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
static unsigned unlock_calls;
struct Ui { bool locked = true, consuming = false; };
bool pxsys_reference_lvgl_handle_gamepad(Ui* ui, uint32_t buttons) {
    ++unlock_calls;
    bool consumed = ui->locked || ui->consuming;
    if (ui->locked && buttons && !ui->consuming) ui->locked = false;
    ui->consuming = consumed && buttons;
    return consumed;
}
void lv_display_trigger_activity(void*) {}
void lv_obj_set_hidden(void*, bool) {}
struct Navigation { void Cancel() {} void Update(void*, bool, int, uint32_t) {} };
struct Samples { bool active() { return false; } };
struct Board {
    Ui ui, *ui_ = &ui;
    void* display_ = nullptr;
    void* cursor_ = nullptr;
    bool screen_on_ = false, swallow_until_release_ = false, gamepad_consumed_ = false;
    bool pointer_down_ = false, touch_pressed_ = false, touch_blocked_ = false;
    uint32_t previous_delivered_ = 0;
    Navigation navigation_;
    Samples touch_samples_;
    struct {} shortcuts_;
    void ApplyBacklight() {}
    void CancelTouch() {}
    void ReportController(uint32_t buttons, uint32_t) { assert(buttons == 0); }
    void Poll(uint32_t gamepad_buttons, bool injected = false) {
        const bool screen_was_off = !screen_on_;
        const uint32_t now = 0;
        if (injected) screen_on_ = true; // PXADB wake_requested_ is drained first.
        /*POLICY*/
    }
};
int main() {
    for (bool injected : {false, true}) {
        Board board;
        const auto before = unlock_calls;
        board.Poll(16, injected);
        assert(board.screen_on_ && board.ui.locked);
        board.Poll(16); board.Poll(16 | 32); board.Poll(32);
        assert(board.ui.locked && unlock_calls == before);
        board.Poll(0);
        assert(board.ui.locked && !board.swallow_until_release_);
        board.Poll(16);
        assert(!board.ui.locked && board.gamepad_consumed_);
        board.Poll(16 | 64);
        assert(board.gamepad_consumed_);
        board.Poll(0);
        assert(board.gamepad_consumed_);
        board.Poll(0);
        assert(!board.gamepad_consumed_);
    }
    Board lit;
    lit.screen_on_ = true;
    lit.Poll(16);
    assert(!lit.ui.locked); // Already-lit lock screen still unlocks in one press.
    puts("wake input: physical/injected press only wakes, holds/joins/releases swallowed; next press unlocks");
}
'''


def main():
    boards = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="gamekit-wake-") as directory:
        directory = Path(directory)
        for board in ("doit-gamekit", "doit-gamekit-1"):
            source = next((boards / board / "src").glob("*_board.cc")).read_text()
            start = source.index("    if (screen_was_off &&", source.index("void Gamekit::PollInput()"))
            end = source.index("    if (gamepad_buttons) lv_display_trigger_activity", start)
            generated = SHIM.replace("/*POLICY*/", source[start:end])
            # std::initializer_list supports the physical/injected case table.
            generated = "#include <initializer_list>\n" + generated
            test = directory / "wake.cc"
            test.write_text(generated)
            binary = directory / "wake"
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(board, "passed", flush=True)


if __name__ == "__main__":
    main()
