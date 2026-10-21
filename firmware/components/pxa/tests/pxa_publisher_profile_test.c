#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pxa_publisher_profile.h"
int main(void) {
    pxa_publisher_profile_t profile;
    const char *valid = "{\"schema\":\"pxa.publisher-profile/"
                        "1\",\"handle\":\"lucinhu\",\"display_name\":\"作者 Lucin\"}\n";
    assert(pxa_publisher_profile_parse((const uint8_t *)valid, strlen(valid), &profile) ==
           PXA_STATUS_OK);
    assert(!strcmp(profile.handle, "lucinhu") && !strcmp(profile.display_name, "作者 Lucin"));
    const char *bad[] = {
        "{}",
        "[]",
        "{\"schema\":\"pxa.publisher-profile/"
        "1\",\"handle\":\"lucinhu\",\"handle\":\"fake\",\"display_name\":\"x\"}",
        "{\"schema\":\"pxa.publisher-profile/1\",\"handle\":\"con\",\"display_name\":\"x\"}",
        "{\"schema\":\"pxa.publisher-profile/1\",\"handle\":\"../x\",\"display_name\":\"x\"}",
        "{\"schema\":\"pxa.publisher-profile/1\",\"handle\":\"a--b\",\"display_name\":\"x\"}",
        "{\"schema\":\"pxa.publisher-profile/1\",\"handle\":\"a\",\"display_name\":\"x\\u202ey\"}",
        "{\"schema\":\"pxa.publisher-profile/"
        "1\",\"handle\":\"a\",\"display_name\":\"x\",\"verified\":\"yes\"}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&profile, 255, sizeof(profile));
        assert(pxa_publisher_profile_parse((const uint8_t *)bad[i], strlen(bad[i]), &profile) !=
               PXA_STATUS_OK);
        assert(!profile.handle[0]);
    }
    const char *trim = "{\"schema\":\"pxa.publisher-profile/"
                       "1\",\"handle\":\"lucinhu\",\"display_name\":\"　作者  \"}";
    assert(pxa_publisher_profile_parse((const uint8_t *)trim, strlen(trim), &profile) ==
               PXA_STATUS_OK &&
           !strcmp(profile.display_name, "作者"));
    const char *space =
        "{\"schema\":\"pxa.publisher-profile/1\",\"handle\":\"lucinhu\",\"display_name\":\"　 \"}";
    assert(pxa_publisher_profile_parse((const uint8_t *)space, strlen(space), &profile) !=
           PXA_STATUS_OK);
    const char *format = "{\"schema\":\"pxa.publisher-profile/"
                         "1\",\"handle\":\"lucinhu\",\"display_name\":\"x\\u0600y\"}";
    assert(pxa_publisher_profile_parse((const uint8_t *)format, strlen(format), &profile) !=
           PXA_STATUS_OK);
    const char *nul = "{\"schema\":\"pxa.publisher-profile/"
                      "1\",\"handle\":\"lucinhu\\u0000fake\",\"display_name\":\"x\"}";
    assert(pxa_publisher_profile_parse((const uint8_t *)nul, strlen(nul), &profile) !=
           PXA_STATUS_OK);
    for (size_t i = 0; i < strlen(valid) - 2; ++i)
        assert(pxa_publisher_profile_parse((const uint8_t *)valid, i, &profile) != PXA_STATUS_OK);
    puts("PASS: bounded signed display profile, CJK, malformed/truncated JSON, duplicate/unknown "
         "fields, reserved names and bidi controls");
}
