#include "pxa_host_activation_arena.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PXA_HOST_ACTIVATION_BLOCK_BYTES (16u * 1024u)
#define PXA_HOST_ACTIVATION_PAGE_BYTES 4096u
#define PXA_HOST_ACTIVATION_ALIGNMENT 16u

static int arena_valid(const pxa_host_activation_arena_t *arena) {
    return arena != NULL && arena->allocate != NULL && arena->release != NULL &&
           (arena->usage.block_count == 0 || arena->blocks != NULL) &&
           arena->usage.block_count <= arena->block_capacity;
}

static int reserve_blocks(pxa_host_activation_arena_t *arena,
                          size_t required) {
    pxa_host_activation_block_t *resized;
    size_t capacity;
    if (arena == NULL || required == 0) return 0;
    if (required <= arena->block_capacity) return 1;
    capacity = arena->block_capacity == 0 ? 4 : arena->block_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*resized)) return 0;
    resized = realloc(arena->blocks, capacity * sizeof(*resized));
    if (resized == NULL) return 0;
    memset(resized + arena->block_capacity, 0,
           (capacity - arena->block_capacity) * sizeof(*resized));
    arena->blocks = resized;
    arena->block_capacity = capacity;
    return 1;
}

static void update_peaks(pxa_host_activation_arena_t *arena) {
    pxa_host_activation_memory_snapshot_t *usage = &arena->usage;
    if (usage->reserved_bytes > usage->peak_reserved_bytes)
        usage->peak_reserved_bytes = usage->reserved_bytes;
    if (usage->used_bytes > usage->peak_used_bytes)
        usage->peak_used_bytes = usage->used_bytes;
}

static void *allocate_from_block(pxa_host_activation_block_t *block,
                                 size_t size, size_t *consumed_bytes) {
    const uintptr_t mask = PXA_HOST_ACTIVATION_ALIGNMENT - 1u;
    uintptr_t base;
    uintptr_t cursor;
    uintptr_t aligned;
    size_t offset;
    size_t previous_used;
    if (block == NULL || !block->sliceable || block->memory == NULL ||
        block->used > block->size || consumed_bytes == NULL) {
        return NULL;
    }
    base = (uintptr_t)block->memory;
    previous_used = block->used;
    if (base > UINTPTR_MAX - previous_used) return NULL;
    cursor = base + previous_used;
    if (cursor > UINTPTR_MAX - mask) return NULL;
    aligned = (cursor + mask) & ~mask;
    if (aligned < base) return NULL;
    offset = (size_t)(aligned - base);
    if (offset > block->size || size > block->size - offset) return NULL;
    block->used = offset + size;
    *consumed_bytes = block->used - previous_used;
    return (void *)aligned;
}

static int block_capacity(size_t size, size_t *output) {
    const size_t alignment_slack = PXA_HOST_ACTIVATION_ALIGNMENT - 1u;
    const size_t page_mask = PXA_HOST_ACTIVATION_PAGE_BYTES - 1u;
    size_t required;
    if (output == NULL || size == 0 ||
        size > SIZE_MAX - alignment_slack) {
        return 0;
    }
    required = size + alignment_slack;
    if (required <= PXA_HOST_ACTIVATION_BLOCK_BYTES) {
        *output = PXA_HOST_ACTIVATION_BLOCK_BYTES;
        return 1;
    }
    if (required > SIZE_MAX - page_mask) return 0;
    *output = (required + page_mask) & ~page_mask;
    return *output >= required;
}

int pxa_host_activation_arena_init(
    pxa_host_activation_arena_t *arena, void *allocator_context,
    pxa_host_activation_allocate_fn allocate,
    pxa_host_activation_release_fn release) {
    if (arena == NULL || allocate == NULL || release == NULL) return 0;
    memset(arena, 0, sizeof(*arena));
    arena->allocator_context = allocator_context;
    arena->allocate = allocate;
    arena->release = release;
    return 1;
}

int pxa_host_activation_arena_begin(
    pxa_host_activation_arena_t *arena, void *first, size_t first_size,
    void *second, size_t second_size) {
    pxa_host_activation_memory_snapshot_t *usage;
    if (!arena_valid(arena) || arena->usage.block_count != 0 || first == NULL ||
        first_size == 0 || second == NULL || second_size == 0 ||
        second_size > SIZE_MAX - first_size) {
        return 0;
    }
    if (!reserve_blocks(arena, 2)) return 0;
    memset(arena->blocks, 0,
           arena->block_capacity * sizeof(arena->blocks[0]));
    arena->blocks[0].memory = first;
    arena->blocks[0].size = first_size;
    arena->blocks[0].used = first_size;
    arena->blocks[1].memory = second;
    arena->blocks[1].size = second_size;
    arena->blocks[1].used = second_size;
    usage = &arena->usage;
    memset(usage, 0, sizeof(*usage));
    usage->block_count = 2;
    usage->reserved_bytes = first_size + second_size;
    usage->used_bytes = usage->reserved_bytes;
    update_peaks(arena);
    return 1;
}

void *pxa_host_activation_arena_allocate(
    pxa_host_activation_arena_t *arena, size_t size) {
    pxa_host_activation_memory_snapshot_t *usage;
    pxa_host_activation_block_t *block;
    void *memory;
    size_t consumed;
    size_t capacity;
    size_t index;
    if (!arena_valid(arena) || size == 0) return NULL;
    usage = &arena->usage;
    if (usage->workspace_count == UINT16_MAX ||
        size > SIZE_MAX - usage->used_bytes) {
        return NULL;
    }
    for (index = 0; index < usage->block_count; ++index) {
        memory = allocate_from_block(&arena->blocks[index], size, &consumed);
        if (memory == NULL) continue;
        usage->used_bytes += consumed;
        usage->workspace_count++;
        update_peaks(arena);
        return memory;
    }
    if (!block_capacity(size, &capacity) ||
        capacity > SIZE_MAX - usage->reserved_bytes) {
        return NULL;
    }
    if (!reserve_blocks(arena, usage->block_count + 1u)) return NULL;
    memory = arena->allocate(arena->allocator_context, capacity);
    if (memory == NULL) return NULL;
    block = &arena->blocks[usage->block_count];
    memset(block, 0, sizeof(*block));
    block->memory = memory;
    block->size = capacity;
    block->sliceable = 1;
    usage->block_count++;
    usage->reserved_bytes += capacity;
    memory = allocate_from_block(block, size, &consumed);
    if (memory == NULL) {
        usage->block_count--;
        usage->reserved_bytes -= capacity;
        arena->release(arena->allocator_context, block->memory);
        memset(block, 0, sizeof(*block));
        return NULL;
    }
    usage->used_bytes += consumed;
    usage->workspace_count++;
    update_peaks(arena);
    return memory;
}

void pxa_host_activation_arena_snapshot(
    const pxa_host_activation_arena_t *arena,
    pxa_host_activation_memory_snapshot_t *output) {
    if (output == NULL) return;
    memset(output, 0, sizeof(*output));
    if (!arena_valid(arena)) return;
    *output = arena->usage;
}

void pxa_host_activation_arena_release_all(
    pxa_host_activation_arena_t *arena) {
    pxa_host_activation_allocate_fn allocate;
    pxa_host_activation_release_fn release;
    void *allocator_context;
    if (!arena_valid(arena)) return;
    allocate = arena->allocate;
    release = arena->release;
    allocator_context = arena->allocator_context;
    while (arena->usage.block_count != 0) {
        pxa_host_activation_block_t *block =
            &arena->blocks[--arena->usage.block_count];
        release(allocator_context, block->memory);
        memset(block, 0, sizeof(*block));
    }
    free(arena->blocks);
    arena->blocks = NULL;
    arena->block_capacity = 0;
    memset(&arena->usage, 0, sizeof(arena->usage));
    arena->allocate = allocate;
    arena->release = release;
    arena->allocator_context = allocator_context;
}
