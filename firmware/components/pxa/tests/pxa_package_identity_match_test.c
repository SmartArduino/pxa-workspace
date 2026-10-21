#include <assert.h>
#include <stdio.h>
#include "pxa_package_identity_match.h"
int main(void) {
    const unsigned char a[32] = {1}, b[32] = {2};
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        pxa_package_identity_match_t m = {.query = "pxa-nes"};
        pxa_package_identity_consider(&m, reverse ? "b:pxa-nes" : "a:pxa-nes", "pxa-nes",
                                      reverse ? b : a);
        assert(m.matches == 1 && m.identity[0]);
        pxa_package_identity_consider(&m, reverse ? "a:pxa-nes" : "b:pxa-nes", "pxa-nes",
                                      reverse ? a : b);
        assert(m.matches == 2 && !m.identity[0] && !m.app_id[0] && !m.publisher_root[0]);
        m = (pxa_package_identity_match_t){.query = "a:pxa-nes"};
        pxa_package_identity_consider(&m, "b:pxa-nes", "pxa-nes", b);
        pxa_package_identity_consider(&m, "a:pxa-nes", "pxa-nes", a);
        assert(m.matches == 1 && !strcmp(m.identity, "a:pxa-nes") && m.publisher_root[0] == 1);
        m = (pxa_package_identity_match_t){.query = "not-installed"};
        pxa_package_identity_consider(&m, "a:pxa-nes", "pxa-nes", a);
        assert(!m.matches);
    }
    puts("PASS: exact owner, unique legacy name, ambiguity in either order, no first-candidate "
         "leak");
}
