#include "pxa_package_disabled_policy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t bytes[512];
    size_t size;
    unsigned calls;
    int fail;
} persistence_t;

static void *test_reallocate(void *context, void *memory, size_t size) {
    (void)context;
    return realloc(memory, size);
}

static void test_release(void *context, void *memory) {
    (void)context;
    free(memory);
}

static bool test_persist(void *context, const uint8_t *bytes, size_t size) {
    persistence_t *persistence = context;
    ++persistence->calls;
    if (persistence->fail || size > sizeof(persistence->bytes)) return false;
    if (size != 0) memcpy(persistence->bytes, bytes, size);
    persistence->size = size;
    return true;
}

static pxa_package_disabled_policy_t make_policy(persistence_t *persistence,
                                                 size_t max_entries) {
    pxa_package_disabled_policy_t policy;
    const pxa_package_disabled_policy_config_t config = {
        .max_entries = max_entries,
        .growth = 2,
        .reallocate = test_reallocate,
        .release = test_release,
        .persistence_context = persistence,
        .persist = test_persist,
    };
    assert(pxa_package_disabled_policy_init(&policy, &config));
    return policy;
}

static void expect_persisted(const persistence_t *persistence,
                             const char *expected) {
    const size_t size = strlen(expected);
    assert(persistence->size == size);
    assert(memcmp(persistence->bytes, expected, size) == 0);
}

static void test_load_sorts_deduplicates_and_filters(void) {
    static const uint8_t stored[] =
        "z.app\na.app\nbad/id\na.app\nmissing-newline";
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, 8);
    assert(pxa_package_disabled_policy_load(&policy, stored,
                                            sizeof(stored) - 1u));
    assert(policy.count == 3);
    assert(policy.capacity == 4);
    assert(pxa_package_disabled_policy_contains(&policy, "a.app"));
    assert(pxa_package_disabled_policy_contains(&policy, "missing-newline"));
    assert(pxa_package_disabled_policy_contains(&policy, "z.app"));
    assert(!pxa_package_disabled_policy_contains(&policy, "bad/id"));
    pxa_package_disabled_policy_deinit(&policy);
}

static void test_updates_are_sorted_and_skip_noops(void) {
    static const uint8_t stored[] = "b.app\nd.app\n";
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, 8);
    assert(pxa_package_disabled_policy_load(&policy, stored,
                                            sizeof(stored) - 1u));
    assert(pxa_package_disabled_policy_set_enabled(&policy, "c.app", false));
    expect_persisted(&persistence, "b.app\nc.app\nd.app\n");
    assert(pxa_package_disabled_policy_contains(&policy, "c.app"));
    assert(persistence.calls == 1);
    assert(pxa_package_disabled_policy_set_enabled(&policy, "c.app", false));
    assert(persistence.calls == 1);
    assert(pxa_package_disabled_policy_set_enabled(&policy, "b.app", true));
    expect_persisted(&persistence, "c.app\nd.app\n");
    assert(!pxa_package_disabled_policy_contains(&policy, "b.app"));
    pxa_package_disabled_policy_deinit(&policy);
}

static void test_failed_persistence_does_not_publish(void) {
    static const uint8_t stored[] = "a.app\n";
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, 4);
    assert(pxa_package_disabled_policy_load(&policy, stored,
                                            sizeof(stored) - 1u));
    persistence.fail = 1;
    assert(!pxa_package_disabled_policy_set_enabled(&policy, "b.app", false));
    assert(!pxa_package_disabled_policy_contains(&policy, "b.app"));
    assert(!pxa_package_disabled_policy_set_enabled(&policy, "a.app", true));
    assert(pxa_package_disabled_policy_contains(&policy, "a.app"));
    pxa_package_disabled_policy_deinit(&policy);
}

static void test_capacity_and_identity_limits(void) {
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, 2);
    assert(pxa_package_disabled_policy_set_enabled(&policy, "a.app", false));
    assert(pxa_package_disabled_policy_set_enabled(&policy, "b.app", false));
    assert(!pxa_package_disabled_policy_set_enabled(&policy, "c.app", false));
    assert(!pxa_package_disabled_policy_set_enabled(&policy, "Bad.App", false));
    assert(policy.count == 2);
    assert(persistence.calls == 2);
    pxa_package_disabled_policy_deinit(&policy);
}

static void test_composite_identity(void) {
    static const char identity[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
        "com.example.shared";
    static const char other_identity[] =
        "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210:"
        "com.example.shared";
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, 4);
    assert(pxa_package_disabled_policy_set_enabled(&policy, identity, false));
    assert(pxa_package_disabled_policy_set_enabled(&policy, other_identity,
                                                   false));
    assert(pxa_package_disabled_policy_contains(&policy, identity));
    assert(pxa_package_disabled_policy_contains(&policy, other_identity));
    assert(!pxa_package_disabled_policy_set_enabled(
        &policy,
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg:"
        "com.example.shared",
        false));
    assert(!pxa_package_disabled_policy_set_enabled(
        &policy,
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
        "Bad.App",
        false));
    pxa_package_disabled_policy_deinit(&policy);
}

static void test_enabling_last_entry_persists_empty_set(void) {
    static const uint8_t stored[] = "only.app\n";
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, 2);
    assert(pxa_package_disabled_policy_load(&policy, stored,
                                            sizeof(stored) - 1u));
    assert(pxa_package_disabled_policy_set_enabled(&policy, "only.app", true));
    assert(persistence.calls == 1);
    assert(persistence.size == 0);
    assert(policy.count == 0);
    assert(!pxa_package_disabled_policy_contains(&policy, "only.app"));
    pxa_package_disabled_policy_deinit(&policy);
}

static void test_dynamic_capacity_exceeds_legacy_package_limit(void) {
    persistence_t persistence = {0};
    pxa_package_disabled_policy_t policy = make_policy(&persistence, SIZE_MAX);
    unsigned index;
    for (index = 0; index < 65; ++index) {
        char identity[16];
        assert(snprintf(identity, sizeof(identity), "app%u", index) > 0);
        assert(pxa_package_disabled_policy_set_enabled(&policy, identity,
                                                       false));
    }
    assert(policy.count == 65);
    assert(policy.capacity >= policy.count);
    pxa_package_disabled_policy_deinit(&policy);
}

int main(void) {
    test_load_sorts_deduplicates_and_filters();
    test_updates_are_sorted_and_skip_noops();
    test_failed_persistence_does_not_publish();
    test_capacity_and_identity_limits();
    test_composite_identity();
    test_enabling_last_entry_persists_empty_set();
    test_dynamic_capacity_exceeds_legacy_package_limit();
    return 0;
}
