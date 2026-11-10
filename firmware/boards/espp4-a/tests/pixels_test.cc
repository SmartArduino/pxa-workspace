// SPDX-License-Identifier: Apache-2.0
#include "pixels_cases.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(TestEspp4Pixels());
    std::puts("PASS: P4 pixel alignment, expansion, tails and destination guards");
}
