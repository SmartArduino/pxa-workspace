#include "doit_gamekit_1_input.h"
#include <cassert>
#include <cstdint>

using namespace doit_gamekit_1;

int main() {
    // A paused emulator hides its Surface but retains a root controller listener.
    assert(!NativeNavigation(false, false, true, false));
    assert(!NativeNavigation(false, false, true, true));
    assert(NativeNavigation(false, true, true, false)); // Permission / IME / recents.
    assert(NativeNavigation(false, true, true, true));
    assert(NativeNavigation(false, false, false, false)); // Native launcher.
    assert(!NativeNavigation(false, false, false, true)); // Legacy game Surface.
    assert(!NativeNavigation(true, true, true, false)); // Explicit cursor mode.
    // Simultaneous game and system keys debounce independently, including bit 13.
    Debouncer keys;
    assert(keys.Update(kButtonMask, 100) == 0);
    assert(keys.Update(kButtonMask, 111) == 0);
    assert(keys.Update(kButtonMask, 112) == kButtonMask);
    assert(keys.Update(A | Return, 113) == kButtonMask);
    assert(keys.Update(A | Return, 125) == (A | Return));
    assert(keys.Update(A, 126) == (A | Return));
    assert(keys.Update(A | Return, 127) == (A | Return));
    assert(keys.Update(A, 128) == (A | Return));
    assert(keys.Update(A, 140) == A);
    Debouncer wrapping;
    assert(wrapping.Update(Return, UINT32_MAX - 5) == 0);
    assert(wrapping.Update(Return, 6) == Return);
    assert(wrapping.Update(Return | (1u << 31), 7) == Return);

    Shortcuts system;
    auto result = system.Update(Menu | Return | A, 100);
    assert(result.back && !result.home && !result.recents && result.buttons == 0);
    assert(!system.Update(Menu | Return | A, 200).back);
    result = system.Update(A, 300);
    assert(result.recents && !result.home && result.buttons == A);
    assert(!system.Update(A, 301).recents);
    assert(system.Update(Return, 302).back);
    (void)system.Update(0, 303);

    // A long MENU press emits Home once, with no trailing Recents on release.
    assert(!system.Update(Menu, 1000).home);
    assert(!system.Update(Menu, 1599).home);
    assert(system.Update(Menu, 1600).home);
    assert(!system.Update(Menu, 2000).home);
    assert(!system.Update(0, 2001).recents);
    (void)system.Update(Menu, UINT32_MAX - 299);
    assert(!system.Update(Menu, 299).home);
    assert(system.Update(Menu, 300).home);
    assert(!system.Update(0, 301).recents);

    // The existing SELECT shortcuts still consume their full press sequence.
    Shortcuts shortcuts;
    assert(shortcuts.Update(kControllerMask, 0).buttons == 0);
    (void)shortcuts.Update(0, 1);
    assert(shortcuts.Update(A | B | X | Y | L | R, 2).buttons == (A | B | X | Y | L | R));
    assert(!shortcuts.Update(Select | Start, 10).home);
    assert(!shortcuts.Update(Select | Start, 609).home);
    assert(shortcuts.Update(Select | Start, 610).home);
    assert(!shortcuts.Update(Select | Start, 900).home);
    assert(shortcuts.Update(Start, 901).buttons == 0);
    (void)shortcuts.Update(0, 902);
    assert(!shortcuts.Update(Select | X, 1000).toggle_pointer);
    assert(!shortcuts.Update(Select | X, 1001).toggle_pointer);
    assert(shortcuts.Update(X | A, 1002).buttons == (X | A));
    (void)shortcuts.Update(0, 1003);
    assert(shortcuts.Update(Select | L, 2000).volume_delta == -5);
    assert(shortcuts.Update(Select | L, 2249).volume_delta == 0);
    assert(shortcuts.Update(Select | L, 2250).volume_delta == -5);
    (void)shortcuts.Update(0, 2251);
    assert(shortcuts.Update(Select | R, 2300).volume_delta == 5);
    (void)shortcuts.Update(0, 2301);
    assert(!shortcuts.Update(Select | Y, 3000).provision_wifi);
    assert(!shortcuts.Update(Select | Y, 4199).provision_wifi);
    assert(shortcuts.Update(Select | Y, 4200).provision_wifi);
    assert(!shortcuts.Update(Select | Y, 4201).provision_wifi);
    (void)shortcuts.Update(0, 4202);
    assert(shortcuts.Update(Menu | Return, 4300).buttons == 0);

    Shortcuts game_keys;
    for (const uint32_t chord : {Select | Start, Select | L, Select | R, Select | X, Select | Y}) {
        auto action = game_keys.Update(chord, 0, false);
        assert(action.buttons == chord && !action.volume_delta && !action.home &&
               !action.toggle_pointer && !action.provision_wifi);
        action = game_keys.Update(chord, 5000, false);
        assert(action.buttons == chord && !action.home && !action.provision_wifi);
        (void)game_keys.Update(0, 5001, false);
    }
    auto volume = game_keys.Update(Menu | R, 6000, false);
    assert(volume.volume_delta == 5 && volume.buttons == 0 && !volume.recents);
    assert(!game_keys.Update(Menu | R, 6249, false).volume_delta);
    assert(game_keys.Update(Menu | R, 6250, false).volume_delta == 5);
    assert(!game_keys.Update(Menu | R, 7000, false).home);
    assert(!game_keys.Update(R, 7001, false).recents);
    assert(game_keys.Update(R, 7002, false).buttons == 0); // no trailing game R
    (void)game_keys.Update(0, 7003, false);
    volume = game_keys.Update(Menu | L, 7100, false);
    assert(volume.volume_delta == -5 && volume.buttons == 0);
    (void)game_keys.Update(0, 7101, false);
    assert(game_keys.Update(A | R, 7102, false).buttons == (A | R));
    auto mouse = game_keys.Update(Menu | X, 8000, false);
    assert(mouse.toggle_pointer && mouse.buttons == 0 && !mouse.recents);
    mouse = game_keys.Update(Menu | X, 9000, false);
    assert(!mouse.toggle_pointer && !mouse.home);
    assert(!game_keys.Update(X, 9001, false).recents);
    assert(game_keys.Update(X, 9002, false).buttons == 0);
    (void)game_keys.Update(0, 9003, false);
    assert(game_keys.Update(X, 9004, false).buttons == X);
}
