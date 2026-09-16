#include "pxa_host_activation_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned allocations;
    unsigned releases;
} allocation_probe_t;

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void *probe_allocate(void *context, size_t size) {
    allocation_probe_t *probe = (allocation_probe_t *)context;
    void *memory = malloc(size);
    if (memory != NULL) probe->allocations++;
    return memory;
}

static void probe_release(void *context, void *memory) {
    allocation_probe_t *probe = (allocation_probe_t *)context;
    if (memory != NULL) probe->releases++;
    free(memory);
}

int main(void) {
    pxa_host_activation_arena_t arena;
    pxa_host_activation_memory_snapshot_t usage;
    allocation_probe_t probe = {0, 0};
    void *manifest = malloc(1024);
    void *encoded = malloc(4096);
    void *small[12];
    void *large[30];
    size_t index;

    CHECK(manifest != NULL);
    CHECK(encoded != NULL);
    CHECK(pxa_host_activation_arena_init(
        &arena, &probe, probe_allocate, probe_release));
    CHECK(pxa_host_activation_arena_begin(
        &arena, manifest, 1024, encoded, 4096));
    for (index = 0; index < sizeof(small) / sizeof(small[0]); ++index) {
        small[index] = pxa_host_activation_arena_allocate(&arena, 256);
        CHECK(small[index] != NULL);
        CHECK(((uintptr_t)small[index] & 15u) == 0);
        if (small[index] != NULL) memset(small[index], (int)index, 256);
    }
    for (index = 0; index < sizeof(large) / sizeof(large[0]); ++index) {
        large[index] = pxa_host_activation_arena_allocate(&arena, 20000);
        CHECK(large[index] != NULL);
        CHECK(((uintptr_t)large[index] & 15u) == 0);
    }
    pxa_host_activation_arena_snapshot(&arena, &usage);
    CHECK(usage.workspace_count == 42);
    CHECK(usage.block_count > 24);
    CHECK(usage.block_count < usage.workspace_count);
    CHECK(usage.used_bytes <= usage.reserved_bytes);
    CHECK(usage.peak_used_bytes == usage.used_bytes);
    CHECK(usage.peak_reserved_bytes == usage.reserved_bytes);
    pxa_host_activation_arena_release_all(&arena);
    CHECK(probe.releases == probe.allocations + 2);
    pxa_host_activation_arena_snapshot(&arena, &usage);
    CHECK(usage.block_count == 0);
    CHECK(usage.used_bytes == 0);

    manifest = malloc(64);
    encoded = malloc(64);
    CHECK(manifest != NULL);
    CHECK(encoded != NULL);
    CHECK(pxa_host_activation_arena_begin(&arena, manifest, 64, encoded, 64));
    CHECK(pxa_host_activation_arena_allocate(&arena, 1) != NULL);
    pxa_host_activation_arena_release_all(&arena);
    CHECK(probe.releases == probe.allocations + 4);

    if (failures != 0) return 1;
    printf("pxa_host_activation_arena_test OK\n");
    return 0;
}
