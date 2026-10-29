// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_EXT_VOICE_TRIGGER_PROTOCOL_H
#define PXA_EXT_VOICE_TRIGGER_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

/* Allocation belongs to extensions/registry.json, not the core service ABI. */
#define PXA_VT_SERVICE_ID UINT16_C(33025)
#define PXA_VT_RESOURCE_TYPE UINT16_C(33025)
#define PXA_VT_MAJOR UINT16_C(0)
#define PXA_VT_MINOR UINT16_C(1)
#define PXA_VT_IDENTITY {0xcd,0x34,0x79,0xd1,0x53,0xf1,0x47,0xbc,0xa8,0xcc,0x54,0x87,0x3b,0x70,0x6c,0xb2}
#define PXA_VT_PERMISSION "org.doit.voice-trigger"
#define PXA_VT_DEFAULT_SCOPE "system.default"
#define PXA_VT_DEFAULT_PROFILE UINT32_C(1)
#define PXA_VT_MAX_KEYWORDS 16u
#define PXA_VT_MAX_WORD_BYTES 63u
#define PXA_VT_HELLO UINT16_C(1)
#define PXA_VT_CAPABILITIES UINT16_C(2)
#define PXA_VT_LIST_PROFILES UINT16_C(3)
#define PXA_VT_LIST_KEYWORDS UINT16_C(4)
#define PXA_VT_SUBSCRIBE UINT16_C(5)
#define PXA_VT_EVENT UINT16_C(0x8001)
#define PXA_VT_TRIGGERED UINT32_C(1)
#define PXA_VT_FAILED UINT32_C(2)
#define PXA_VT_FOREGROUND_ONLY UINT32_C(1)
#define PXA_VT_PLAYBACK_DETECTION UINT32_C(2)
#define PXA_VT_PREROLL UINT32_C(4)
#define PXA_VT_FIXED_CATALOG UINT32_C(0)
#define PXA_VT_GRAMMAR_RESOURCE UINT32_C(1)
#define PXA_VT_MODEL_RESOURCE UINT32_C(2)
#define PXA_VT_CAPABILITIES_BYTES 24u
#define PXA_VT_EVENT_BYTES 40u

/* Labels are complete UTF-8 words, never truncated byte prefixes. */
static inline int pxa_vt_valid_word(const char *word, size_t size) {
    size_t i = 0;
    if (!word || !size || size > PXA_VT_MAX_WORD_BYTES) return 0;
    while (i < size) {
        uint32_t cp;
        unsigned trailing, j;
        uint8_t c = (uint8_t)word[i++];
        if (c && c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31u; trailing = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15u; trailing = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7u; trailing = 3; }
        else return 0;
        if (size - i < trailing) return 0;
        for (j = 0; j < trailing; ++j) {
            c = (uint8_t)word[i++];
            if ((c & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (c & 63u);
        }
        if ((trailing == 2 && cp < 0x800) || (trailing == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return 0;
    }
    return 1;
}
#endif
