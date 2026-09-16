#include "pxa_package_disabled_policy.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static bool valid_app_id_bytes(const uint8_t *value, size_t size) {
    size_t index;
    if (value == NULL || size == 0 || size > 64u) return false;
    for (index = 0; index < size; ++index) {
        const uint8_t byte = value[index];
        const bool allowed = (byte >= 'a' && byte <= 'z') ||
                             (byte >= '0' && byte <= '9') || byte == '.' ||
                             byte == '_' || byte == '-';
        if (!allowed) return false;
    }
    return true;
}

static bool valid_identity_bytes(const uint8_t *value, size_t size) {
    size_t index;
    if (value == NULL || size == 0 ||
        size >= PXA_PACKAGE_DISABLED_POLICY_ID_BYTES) {
        return false;
    }
    if (size <= 64u) return valid_app_id_bytes(value, size);
    if (size < 66u || value[64] != ':') return false;
    for (index = 0; index < 64u; ++index) {
        const uint8_t byte = value[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return valid_app_id_bytes(value + 65u, size - 65u);
}

static bool valid_identity(const char *identity) {
    size_t size = 0;
    if (identity == NULL) return false;
    while (size < PXA_PACKAGE_DISABLED_POLICY_ID_BYTES &&
           identity[size] != '\0') {
        ++size;
    }
    return size < PXA_PACKAGE_DISABLED_POLICY_ID_BYTES &&
           valid_identity_bytes((const uint8_t *)identity, size);
}

static size_t lower_bound(const pxa_package_disabled_policy_t *policy,
                          const char *identity, bool *found) {
    size_t begin = 0;
    size_t end = policy->count;
    while (begin < end) {
        const size_t middle = begin + (end - begin) / 2u;
        const int order = strcmp(policy->entries[middle], identity);
        if (order < 0) {
            begin = middle + 1u;
        } else {
            end = middle;
        }
    }
    if (found != NULL) {
        *found = begin < policy->count &&
                 strcmp(policy->entries[begin], identity) == 0;
    }
    return begin;
}

static bool ensure_capacity(pxa_package_disabled_policy_t *policy,
                            size_t required) {
    size_t capacity;
    void *resized;
    if (required <= policy->capacity) return true;
    if (required > policy->config.max_entries) return false;
    capacity = policy->capacity;
    while (capacity < required) {
        const size_t remaining = policy->config.max_entries - capacity;
        const size_t increment =
            remaining < policy->config.growth ? remaining : policy->config.growth;
        if (increment == 0) return false;
        capacity += increment;
    }
    if (capacity > SIZE_MAX / sizeof(policy->entries[0])) return false;
    resized = policy->config.reallocate(
        policy->config.allocator_context, policy->entries,
        capacity * sizeof(policy->entries[0]));
    if (resized == NULL) return false;
    policy->entries = resized;
    policy->capacity = capacity;
    return true;
}

static bool insert_unpersisted(pxa_package_disabled_policy_t *policy,
                               const char *identity) {
    bool found;
    const size_t index = lower_bound(policy, identity, &found);
    if (found) return true;
    if (policy->count >= policy->config.max_entries) return false;
    if (!ensure_capacity(policy, policy->count + 1u)) return false;
    memmove(&policy->entries[index + 1u], &policy->entries[index],
            (policy->count - index) * sizeof(policy->entries[0]));
    snprintf(policy->entries[index], sizeof(policy->entries[index]), "%s",
             identity);
    ++policy->count;
    return true;
}

static bool serialized_size(const pxa_package_disabled_policy_t *policy,
                            size_t removed_index, const char *added,
                            size_t *size_out) {
    size_t size = 0;
    size_t index;
    for (index = 0; index < policy->count; ++index) {
        const size_t entry_size = strlen(policy->entries[index]) + 1u;
        if (index == removed_index) continue;
        if (size > SIZE_MAX - entry_size) return false;
        size += entry_size;
    }
    if (added != NULL) {
        const size_t added_size = strlen(added) + 1u;
        if (size > SIZE_MAX - added_size) return false;
        size += added_size;
    }
    *size_out = size;
    return true;
}

static void append_serialized(uint8_t **cursor, const char *identity) {
    const size_t size = strlen(identity);
    memcpy(*cursor, identity, size);
    (*cursor)[size] = '\n';
    *cursor += size + 1u;
}

static bool persist_candidate(pxa_package_disabled_policy_t *policy,
                              size_t removed_index, const char *added,
                              size_t added_index) {
    size_t size;
    uint8_t *bytes = NULL;
    uint8_t *cursor;
    size_t index;
    bool persisted;
    if (!serialized_size(policy, removed_index, added, &size)) return false;
    if (size != 0) {
        bytes = policy->config.reallocate(policy->config.allocator_context,
                                          NULL, size);
        if (bytes == NULL) return false;
    }
    cursor = bytes;
    for (index = 0; index <= policy->count; ++index) {
        if (added != NULL && index == added_index) {
            append_serialized(&cursor, added);
        }
        if (index < policy->count && index != removed_index) {
            append_serialized(&cursor, policy->entries[index]);
        }
    }
    persisted = policy->config.persist(policy->config.persistence_context,
                                       bytes, size);
    if (bytes != NULL) {
        policy->config.release(policy->config.allocator_context, bytes);
    }
    return persisted;
}

bool pxa_package_disabled_policy_init(
    pxa_package_disabled_policy_t *policy,
    const pxa_package_disabled_policy_config_t *config) {
    if (policy == NULL || config == NULL || config->max_entries == 0 ||
        config->growth == 0 || config->reallocate == NULL ||
        config->release == NULL || config->persist == NULL) {
        return false;
    }
    memset(policy, 0, sizeof(*policy));
    policy->config = *config;
    return true;
}

void pxa_package_disabled_policy_deinit(
    pxa_package_disabled_policy_t *policy) {
    if (policy == NULL) return;
    if (policy->config.release != NULL && policy->entries != NULL) {
        policy->config.release(policy->config.allocator_context,
                               policy->entries);
    }
    memset(policy, 0, sizeof(*policy));
}

bool pxa_package_disabled_policy_load(
    pxa_package_disabled_policy_t *policy, const uint8_t *bytes, size_t size) {
    pxa_package_disabled_policy_t candidate;
    size_t offset = 0;
    if (policy == NULL || policy->config.reallocate == NULL ||
        (bytes == NULL && size != 0)) {
        return false;
    }
    candidate = *policy;
    candidate.entries = NULL;
    candidate.count = 0;
    candidate.capacity = 0;
    while (offset < size && candidate.count < candidate.config.max_entries) {
        char identity[PXA_PACKAGE_DISABLED_POLICY_ID_BYTES];
        size_t end = offset;
        size_t identity_size;
        while (end < size && bytes[end] != '\n') ++end;
        identity_size = end - offset;
        if (valid_identity_bytes(bytes + offset, identity_size)) {
            memcpy(identity, bytes + offset, identity_size);
            identity[identity_size] = '\0';
            if (!insert_unpersisted(&candidate, identity)) {
                if (candidate.entries != NULL) {
                    candidate.config.release(
                        candidate.config.allocator_context, candidate.entries);
                }
                return false;
            }
        }
        offset = end < size ? end + 1u : end;
    }
    if (policy->entries != NULL) {
        policy->config.release(policy->config.allocator_context,
                               policy->entries);
    }
    policy->entries = candidate.entries;
    policy->count = candidate.count;
    policy->capacity = candidate.capacity;
    return true;
}

bool pxa_package_disabled_policy_contains(
    const pxa_package_disabled_policy_t *policy, const char *identity) {
    bool found = false;
    if (policy == NULL || !valid_identity(identity)) return false;
    (void)lower_bound(policy, identity, &found);
    return found;
}

bool pxa_package_disabled_policy_set_enabled(
    pxa_package_disabled_policy_t *policy, const char *identity, bool enabled) {
    bool found;
    size_t index;
    if (policy == NULL || policy->config.persist == NULL ||
        !valid_identity(identity)) {
        return false;
    }
    index = lower_bound(policy, identity, &found);
    if (enabled == !found) return true;
    if (!enabled &&
        (policy->count >= policy->config.max_entries ||
         !ensure_capacity(policy, policy->count + 1u))) {
        return false;
    }
    if (!persist_candidate(policy, enabled ? index : SIZE_MAX,
                           enabled ? NULL : identity, index)) {
        return false;
    }
    if (enabled) {
        memmove(&policy->entries[index], &policy->entries[index + 1u],
                (policy->count - index - 1u) * sizeof(policy->entries[0]));
        --policy->count;
        memset(&policy->entries[policy->count], 0,
               sizeof(policy->entries[0]));
    } else {
        memmove(&policy->entries[index + 1u], &policy->entries[index],
                (policy->count - index) * sizeof(policy->entries[0]));
        snprintf(policy->entries[index], sizeof(policy->entries[index]), "%s",
                 identity);
        ++policy->count;
    }
    return true;
}
