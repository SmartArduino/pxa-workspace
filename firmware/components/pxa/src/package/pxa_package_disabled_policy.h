#ifndef PXA_PACKAGE_DISABLED_POLICY_H
#define PXA_PACKAGE_DISABLED_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 64 publisher-root hex bytes, ':', 64-byte App ID, NUL. */
#define PXA_PACKAGE_DISABLED_POLICY_ID_BYTES 130u

typedef void *(*pxa_package_disabled_policy_reallocate_fn)(
    void *context, void *memory, size_t size);
typedef void (*pxa_package_disabled_policy_release_fn)(void *context,
                                                       void *memory);
typedef bool (*pxa_package_disabled_policy_persist_fn)(
    void *context, const uint8_t *bytes, size_t size);

typedef struct {
    size_t max_entries;
    size_t growth;
    void *allocator_context;
    pxa_package_disabled_policy_reallocate_fn reallocate;
    pxa_package_disabled_policy_release_fn release;
    void *persistence_context;
    pxa_package_disabled_policy_persist_fn persist;
} pxa_package_disabled_policy_config_t;

typedef struct {
    char (*entries)[PXA_PACKAGE_DISABLED_POLICY_ID_BYTES];
    size_t count;
    size_t capacity;
    pxa_package_disabled_policy_config_t config;
} pxa_package_disabled_policy_t;

bool pxa_package_disabled_policy_init(
    pxa_package_disabled_policy_t *policy,
    const pxa_package_disabled_policy_config_t *config);
void pxa_package_disabled_policy_deinit(
    pxa_package_disabled_policy_t *policy);

/* Replaces the in-memory set from newline-delimited storage bytes. Invalid
 * identities are ignored; valid identities are sorted and deduplicated. */
bool pxa_package_disabled_policy_load(
    pxa_package_disabled_policy_t *policy, const uint8_t *bytes, size_t size);

bool pxa_package_disabled_policy_contains(
    const pxa_package_disabled_policy_t *policy, const char *identity);

/* Persists the complete candidate set before publishing the RAM mutation. */
bool pxa_package_disabled_policy_set_enabled(
    pxa_package_disabled_policy_t *policy, const char *identity, bool enabled);

#ifdef __cplusplus
}
#endif

#endif
