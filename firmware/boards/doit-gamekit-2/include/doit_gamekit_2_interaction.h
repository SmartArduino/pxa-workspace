#pragma once
// SPDX-License-Identifier: MIT
#include "doit_gamekit_2_config.h"
#include <pxa/pxa_host_interaction.h>
#include <algorithm>
#include <iterator>

namespace doit_gamekit_2 {
// These tables describe the product routes, not PXADB's injected pointers.
inline constexpr pxa_interaction_control_t kPhysicalControls[] = {
    {"up", "上"}, {"down", "下"}, {"left", "左"}, {"right", "右"},
    {"a", "A"}, {"b", "B"}, {"start", "START"}, {"select", "SELECT"},
    {"x", "X"}, {"y", "Y"}, {"l", "L"}, {"r", "R"},
    {"menu", "菜单"}, {"return", "返回"}, {"touchscreen", "触摸屏"},
};
inline constexpr uint32_t kEdges = PXA_INTERACTION_PRESS | PXA_INTERACTION_RELEASE;
inline constexpr pxa_interaction_binding_t kControllerBindings[] = {
    {"up", "up", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"down", "down", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"left", "left", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"right", "right", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"a", "a", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"b", "b", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"start", "start", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"select", "select", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"x", "x", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"y", "y", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"l", "l", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"r", "r", kEdges | PXA_INTERACTION_HOLD, -2, -1},
};
inline constexpr pxa_interaction_binding_t kFocusBindings[] = {
    {"up", "up", kEdges | PXA_INTERACTION_REPEAT, 0, -1},
    {"down", "down", kEdges | PXA_INTERACTION_REPEAT, 0, -1},
    {"left", "left", kEdges | PXA_INTERACTION_REPEAT, 0, -1},
    {"right", "right", kEdges | PXA_INTERACTION_REPEAT, 0, -1},
    {"confirm", "a", kEdges | PXA_INTERACTION_TAP, 0, -1},
    {"back", "b", PXA_INTERACTION_PRESS, 0, -1},
};
inline constexpr pxa_interaction_binding_t kPointerBindings[] = {
    {"up", "up", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"down", "down", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"left", "left", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"right", "right", kEdges | PXA_INTERACTION_HOLD, -2, -1},
    {"primary", "a", kEdges | PXA_INTERACTION_TAP | PXA_INTERACTION_HOLD, -2, -1},
    {"back", "b", PXA_INTERACTION_PRESS, 0, -1},
};
inline constexpr pxa_interaction_binding_t kTouchBindings[] = {
    {"tap", "touchscreen", PXA_INTERACTION_TAP, 0, -1},
};
inline constexpr pxa_interaction_mode_t kPhysicalModes[] = {
    {"controller", "controller", kControllerBindings, std::size(kControllerBindings)},
    {"focus", "focus", kFocusBindings, std::size(kFocusBindings)},
    {"cursor", "pointer", kPointerBindings, std::size(kPointerBindings)},
    {"touch", "touch", kTouchBindings, std::size(kTouchBindings)},
};
inline constexpr pxa_interaction_reserved_t kSystemReservations[] = {
    {"menu", "tap", "system.recents", 0},
    {"menu", "hold", "system.home", 600},
    {"return", "press", "system.back", 0},
};

inline unsigned PhysicalTouchContacts(bool initialized, bool first, bool second) {
    return !initialized || !first ? 0u : second ? 2u : 1u;
}

inline pxa_host_interaction_t InteractionDescription(
    const char* firmware_id, const char* version, const char* build_id,
    bool touch_enabled, unsigned physical_contacts) {
    const int8_t contacts = static_cast<int8_t>(std::min(physical_contacts, 2u));
    const bool touch = touch_enabled && contacts > 0;
    return {
        .struct_size = sizeof(pxa_host_interaction_t),
        .vendor = "unknown", .model = "doit-gamekit-2", .hardware_revision = "unknown",
        .firmware_id = firmware_id, .firmware_version = version, .build_id = build_id,
        .profile_id = "pxa.reference.doit-gamekit-2.inputs", .revision = 1,
        .width = kWidth, .height = kHeight, .shape = "rectangle",
        .touch = static_cast<int8_t>(touch ? 1 : touch_enabled ? -1 : 0),
        // Controller routes preserve chords; focus/cursor policies can consume
        // them. The schema has no per-mode concurrency field, so keep unknown.
        .simultaneous = -1,
        .max_contacts = static_cast<int8_t>(touch ? contacts : touch_enabled ? -1 : 0),
        .controls = kPhysicalControls, .control_count = touch ? 15u : 14u,
        .modes = kPhysicalModes, .mode_count = touch ? 4u : 3u,
        .exclusive = nullptr, .exclusive_count = 0,
        .reserved = kSystemReservations, .reserved_count = std::size(kSystemReservations),
        .runtime_context = nullptr, .context = nullptr,
    };
}
}
