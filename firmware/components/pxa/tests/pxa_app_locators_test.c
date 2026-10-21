#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pxa_app_locators.h"
static pxa_app_locators_t saved;
static int fail;
static pxa_status_t commit(void *context, uint64_t generation, const pxa_locator_publisher_t *p,
                           size_t np, const pxa_locator_app_t *a, size_t na) {
    (void)context;
    if (fail)
        return PXA_STATUS_IO_ERROR;
    return pxa_app_locators_load(&saved, generation, p, np, a, na);
}
int main(void) {
    pxa_app_locators_t registry;
    pxa_app_locators_init(&registry, NULL, commit);
    pxa_app_locators_init(&saved, NULL, commit);
    const uint8_t root[32] = {1}, second[32] = {2}, third[32] = {3};
    char path[130], original[130];
    uint16_t flags;
    assert(sizeof(registry) <= 8192);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", NULL, path, sizeof(path)) ==
           PXA_STATUS_NOT_FOUND);
    assert(pxa_app_locators_reserve(&registry, 1, root, "lucinhu", root, "pxa-nes", root, 3) ==
           PXA_STATUS_OK);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", &flags, path, sizeof(path)) ==
               PXA_STATUS_OK &&
           flags == 3);
    assert(!strcmp(path, "lucinhu/pxa-nes"));
    strcpy(original, path);
    assert(pxa_app_locators_reserve(&registry, 1, root, "renamed", root, "pxa-nes", root, 0) ==
           PXA_STATUS_OK);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", &flags, path, sizeof(path)) ==
               PXA_STATUS_OK &&
           flags == 3 && !strcmp(path, original));
    assert(pxa_app_locators_reserve(&registry, 1, second, "lucinhu", second, "pxa-nes", second,
                                    0) == PXA_STATUS_OK);
    assert(pxa_app_locators_find(&registry, second, "pxa-nes", NULL, path, sizeof(path)) ==
           PXA_STATUS_OK);
    assert(strstr(path, "lucinhu--") == path && strcmp(path, original));
    /* One verified namespace can publish apps signed by different independent keys. */
    assert(pxa_app_locators_reserve(&registry, 2, root, "author", root, "pxa-quake", third, 0) ==
           PXA_STATUS_OK);
    assert(pxa_app_locators_reserve(&registry, 2, root, "author", third, "pxa-lara", third, 0) ==
           PXA_STATUS_OK);
    assert(pxa_app_locators_find(&registry, third, "pxa-lara", NULL, path, sizeof(path)) ==
               PXA_STATUS_OK &&
           !strcmp(path, "author/pxa-lara"));
    assert(pxa_app_locators_find(&registry, root, "pxa-quake", NULL, path, 3) ==
               PXA_STATUS_RESOURCE_LIMIT &&
           !path[0]);
    assert(pxa_app_locators_reserve(&registry, 1, root, "../x", root, "bad", root, 0) ==
           PXA_STATUS_INVALID_ARGUMENT);
    fail = 1;
    assert(pxa_app_locators_reserve(&registry, 1, root, "lucinhu", root, "new-app", root, 0) ==
           PXA_STATUS_IO_ERROR);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", NULL, path, sizeof(path)) ==
           PXA_STATUS_UNAVAILABLE);
    fail = 0;
    assert(pxa_app_locators_load(&registry, saved.generation, saved.publishers,
                                 saved.publisher_count, saved.apps,
                                 saved.app_count) == PXA_STATUS_OK);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", NULL, path, sizeof(path)) ==
               PXA_STATUS_OK &&
           !strcmp(path, original));
    assert(pxa_app_locators_find(&registry, root, "new-app", NULL, path, sizeof(path)) ==
           PXA_STATUS_NOT_FOUND);
    uint8_t encoded[PXA_LOCATOR_ENCODED_MAX];
    size_t encoded_size = 0;
    assert(pxa_app_locators_encode(registry.generation, registry.publishers,
                                   registry.publisher_count, registry.apps, registry.app_count,
                                   encoded, sizeof(encoded), &encoded_size) == PXA_STATUS_OK);
    assert(pxa_app_locators_decode(&registry, encoded, encoded_size) == PXA_STATUS_OK);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", &flags, path, sizeof(path)) ==
               PXA_STATUS_OK &&
           flags == 3 && !strcmp(path, original));
    for (size_t i = 0; i < encoded_size; ++i) {
        encoded[i] ^= 1;
        assert(pxa_app_locators_decode(&registry, encoded, encoded_size) ==
                   PXA_STATUS_PROTOCOL_ERROR &&
               !registry.ready);
        encoded[i] ^= 1;
    }
    for (size_t i = 0; i < encoded_size; ++i)
        assert(pxa_app_locators_decode(&registry, encoded, i) == PXA_STATUS_PROTOCOL_ERROR);
    assert(pxa_app_locators_decode(&registry, encoded, encoded_size) == PXA_STATUS_OK);
    saved.apps[1] = saved.apps[0];
    assert(pxa_app_locators_load(&registry, saved.generation, saved.publishers,
                                 saved.publisher_count, saved.apps,
                                 saved.app_count) == PXA_STATUS_PROTOCOL_ERROR);
    assert(pxa_app_locators_find(&registry, root, "pxa-nes", NULL, path, sizeof(path)) ==
           PXA_STATUS_UNAVAILABLE);
    printf("PASS: %zu-byte bounded locator registry, exact owners, same-name isolation, multi-key "
           "namespace, stable paths and uncertain-commit recovery\n",
           sizeof(registry));
}
