#include "doit_gamekit_input.h"
#include <cassert>
#include <cstdint>

using namespace doit_gamekit;
int main() {
    // A paused emulator hides its Surface but retains a root controller listener.
    assert(!NativeNavigation(false, false, true, false));
    assert(!NativeNavigation(false, false, true, true));
    assert(NativeNavigation(false, true, true, false)); // Permission / IME / recents.
    assert(NativeNavigation(false, true, true, true));
    assert(NativeNavigation(false, false, false, false)); // Native launcher.
    assert(!NativeNavigation(false, false, false, true)); // Legacy game Surface.
    assert(!NativeNavigation(true, true, true, false)); // Explicit cursor mode.
    Debouncer keys;
    assert(keys.Update(Up | A, 100) == 0);
    assert(keys.Update(Up | A, 111) == 0);
    assert(keys.Update(Up | A, 112) == (Up | A));
    assert(keys.Update(Up, 113) == (Up | A));
    assert(keys.Update(Up | A, 115) == (Up | A)); // contact bounce
    assert(keys.Update(Up, 120) == (Up | A));
    assert(keys.Update(Up, 132) == Up); // one key releases independently
    assert(keys.Update(Up | X | Y | L | R, 140) == Up);
    assert(keys.Update(Up | X | Y | L | R, 152) == (Up | X | Y | L | R));
    assert(keys.Update(0, 160) != 0);
    assert(keys.Update(0, 172) == 0);
    Debouncer wrapping;
    assert(wrapping.Update(B, UINT32_MAX - 5) == 0);
    assert(wrapping.Update(B, 6) == B); // debounce spans tick wrap
    assert(wrapping.Update(B | (1u << 31), 7) == B);

    Shortcuts hotkeys;
    assert(hotkeys.Update(A | R | X, 0).buttons == (A | R | X));
    auto home = hotkeys.Update(Select | Start, 10);
    assert(!home.home && home.buttons == 0);
    assert(!hotkeys.Update(Select | Start, 609).home);
    assert(hotkeys.Update(Select | Start, 610).home);
    assert(!hotkeys.Update(Select | Start, 900).home);
    assert(hotkeys.Update(Start, 901).buttons == 0); // no trailing Start
    assert(hotkeys.Update(0, 902).buttons == 0);
    assert(hotkeys.Update(X | Y | L | R, 903).buttons == (X | Y | L | R));

    assert(!hotkeys.Update(Select | X, 1000).toggle_pointer);
    assert(hotkeys.Update(Select | X, 1001).buttons == (Select | X));
    (void)hotkeys.Update(0, 1002);
    auto cursor = hotkeys.Update(L | R, 1100);
    assert(!cursor.toggle_pointer && cursor.buttons == 0);
    assert(!hotkeys.Update(L | R, 1699).toggle_pointer);
    assert(hotkeys.Update(L | R, 1700).toggle_pointer);
    assert(!hotkeys.Update(L | R, 1701).toggle_pointer);
    assert(hotkeys.Update(R | A, 1702).buttons == 0);
    (void)hotkeys.Update(0, 1703);
    assert(hotkeys.Update(A, 1704).buttons == A);

    assert(!hotkeys.Update(Select | Y, 2000).provision_wifi);
    assert(!hotkeys.Update(Select | Y, 3199).provision_wifi);
    assert(hotkeys.Update(Select | Y, 3200).provision_wifi);
    assert(!hotkeys.Update(Select | Y, 3201).provision_wifi);
    (void)hotkeys.Update(0, 3202);
    assert(hotkeys.Update(Select | L, 4000).volume_delta == -5);
    assert(hotkeys.Update(Select | L, 4249).volume_delta == 0);
    assert(hotkeys.Update(Select | L, 4250).volume_delta == -5);
    (void)hotkeys.Update(0, 4251);
    assert(hotkeys.Update(Select | R, 4300).volume_delta == 5);
    (void)hotkeys.Update(0, 4301);

    // Abandoning a long shortcut must never trigger it later.
    assert(!hotkeys.Update(Select | Start, 5000).home);
    assert(!hotkeys.Update(Start, 6000).home);
    (void)hotkeys.Update(0, 6001);
    assert(hotkeys.Update(Start, 6002).buttons == Start);

    Shortcuts game_keys;
    for (const uint32_t chord : {Select | L, Select | R, Select | X, Select | Y, L | R}) {
        auto action = game_keys.Update(chord, 0, false);
        assert(action.buttons == chord && !action.volume_delta &&
               !action.toggle_pointer && !action.provision_wifi);
        action = game_keys.Update(chord, 5000, false);
        assert(action.buttons == chord && !action.provision_wifi);
        (void)game_keys.Update(0, 5001, false);
    }
    assert(game_keys.Update(Select | Start, 6000, false).buttons == (Select | Start));
    auto held_home = game_keys.Update(Select | Start, 6600, false);
    assert(held_home.home && held_home.buttons == 0);
    assert(game_keys.Update(Start, 6601, false).buttons == 0);
    (void)game_keys.Update(0, 6602, false);
    assert(game_keys.Update(Select | Start, 7000, false).buttons == (Select | Start));
    assert(game_keys.Update(Start, 7100, false).buttons == Start);
    assert(!game_keys.Update(0, 7101, false).home);
}
