// SPDX-License-Identifier: MIT
#include "doit_gamekit_2_interaction.h"
#include <cassert>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string_view scenario = argv[1];
    const bool enabled = scenario != "disabled";
    const bool ready = scenario == "two" || scenario == "one" || scenario == "hole";
    const bool first = scenario != "hole";
    const bool second = scenario != "one";
    const unsigned contacts = doit_gamekit_2::PhysicalTouchContacts(ready, first, second);
    // Even if diagnostics provide five injectable pointers, only initialized
    // FT6336U routes can contribute to this count.
    assert(contacts <= 2);
    const auto descriptor = doit_gamekit_2::InteractionDescription(
        "pxa_esp_platform", "0.11.4",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        enabled, contacts);
    assert(pxa_host_interaction_register(&descriptor));
    char output[PXA_INTERACTION_MAX_BYTES + 1];
    size_t bytes = 0;
    assert(pxa_host_interaction_describe(output, sizeof(output), &bytes));
    assert(bytes > 0 && bytes <= PXA_INTERACTION_MAX_BYTES);
    std::cout.write(output, bytes);
}
