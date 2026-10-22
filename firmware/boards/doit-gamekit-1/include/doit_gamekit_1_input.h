#pragma once
#include "doit_gamekit_1_config.h"

namespace doit_gamekit_1 {
// Canvas applications may own controller input without a visible Surface.
// Native modal/IME scopes always take priority over an app subscription.
inline bool NativeNavigation(bool cursor, bool modal, bool app_controller,
                             bool surface_visible) {
    return !cursor && (modal || (!app_controller && !surface_visible));
}

class Debouncer {
public:
    uint32_t Update(uint32_t raw, uint32_t now_ms) {
        raw &= kButtonMask;
        const uint32_t changed = raw ^ candidate_;
        candidate_ = raw;
        for (size_t i = 0; i < kButtonPins.size(); ++i) {
            const uint32_t bit = 1u << i;
            if (changed & bit) changed_at_[i] = now_ms;
            if (now_ms - changed_at_[i] >= 12)
                stable_ = (stable_ & ~bit) | (candidate_ & bit);
        }
        return stable_;
    }
private:
    uint32_t candidate_ = 0, stable_ = 0;
    std::array<uint32_t, kButtonPins.size()> changed_at_{};
};

struct InputActions {
    uint32_t buttons = 0;
    bool home = false, toggle_pointer = false, provision_wifi = false;
    bool back = false, recents = false;
    int volume_delta = 0;
};

// A shortcut consumes its entire sequence until every involved key is up.
// This prevents its trailing release from clicking UI or holding a game key.
class Shortcuts {
public:
    InputActions Update(uint32_t buttons, uint32_t now_ms) {
        InputActions result{.buttons = buttons & kControllerMask};
        // Dedicated system keys work in games too and never enter controller 0.
        const bool menu_down = (buttons & Menu) != 0;
        const bool was_menu_down = (previous_system_ & Menu) != 0;
        result.back = (buttons & Return) && !(previous_system_ & Return);
        if (menu_down && !was_menu_down) {
            menu_since_ = now_ms;
            menu_home_ = false;
        }
        if (menu_down && !menu_home_ && now_ms - menu_since_ >= 600) {
            result.home = true;
            menu_home_ = true;
        }
        result.recents = was_menu_down && !menu_down && !menu_home_;
        previous_system_ = buttons & (Menu | Return);
        if (consumed_ && !(buttons & consumed_)) consumed_ = 0;
        if (!consumed_ && (buttons & Select)) {
            for (uint32_t other : {uint32_t(Start), uint32_t(X), uint32_t(Y),
                                   uint32_t(L), uint32_t(R)}) {
                if (!(buttons & other)) continue;
                consumed_ = Select | other;
                since_ = now_ms;
                fired_ = false;
                break;
            }
        }
        if (!consumed_) return result;
        result.buttons = 0;
        if ((buttons & consumed_) != consumed_) return result;
        const uint32_t elapsed = now_ms - since_;
        if (consumed_ & (L | R)) {
            if (!fired_ || elapsed >= 250) {
                result.volume_delta = (consumed_ & R) ? 5 : -5;
                fired_ = true;
                since_ = now_ms;
            }
        } else if (!fired_) {
            if ((consumed_ & Start) && elapsed >= 600) result.home = true;
            if (consumed_ & X) result.toggle_pointer = true;
            if ((consumed_ & Y) && elapsed >= 1200) result.provision_wifi = true;
            fired_ = result.home || result.toggle_pointer || result.provision_wifi;
        }
        return result;
    }
private:
    uint32_t consumed_ = 0, since_ = 0;
    bool fired_ = false;
    uint32_t previous_system_ = 0, menu_since_ = 0;
    bool menu_home_ = false;
};
}
