#ifndef PXA_HOST_ACTIVATION_ARENA_H
#define PXA_HOST_ACTIVATION_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*pxa_host_activation_allocate_fn)(void *context, size_t size);
typedef void (*pxa_host_activation_release_fn)(void *context, void *memory);

typedef struct {
    void *memory;
    size_t size;
    size_t used;
    uint8_t sliceable;
} pxa_host_activation_block_t;

typedef struct {
    size_t reserved_bytes;
    size_t used_bytes;
    size_t peak_reserved_bytes;
    size_t peak_used_bytes;
    uint16_t workspace_count;
    size_t block_count;
} pxa_host_activation_memory_snapshot_t;

typedef struct {
    pxa_host_activation_block_t *blocks;
    size_t block_capacity;
    pxa_host_activation_allocate_fn allocate;
    pxa_host_activation_release_fn release;
    void *allocator_context;
    pxa_host_activation_memory_snapshot_t usage;
} pxa_host_activation_arena_t;

int pxa_host_activation_arena_init(
    pxa_host_activation_arena_t *arena, void *allocator_context,
    pxa_host_activation_allocate_fn allocate,
    pxa_host_activation_release_fn release);

/* Takes ownership of both blocks atomically on success. */
int pxa_host_activation_arena_begin(
    pxa_host_activation_arena_t *arena, void *first, size_t first_size,
    void *second, size_t second_size);

void *pxa_host_activation_arena_allocate(
    pxa_host_activation_arena_t *arena, size_t size);

void pxa_host_activation_arena_snapshot(
    const pxa_host_activation_arena_t *arena,
    pxa_host_activation_memory_snapshot_t *output);

void pxa_host_activation_arena_release_all(
    pxa_host_activation_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif
