/* SPDX-License-Identifier: MIT */
#include "wasm_export.h"
#include <stdio.h>
#include <stdlib.h>

static int32_t host_grow(wasm_exec_env_t env, uint32_t pages)
{
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(env);
    void *before = wasm_runtime_addr_app_to_native(instance, 4096);
    void *blocker = malloc(65536);
    bool result = wasm_runtime_enlarge_memory(instance, pages);
    void *after = wasm_runtime_addr_app_to_native(instance, 4096);
    fprintf(stderr, "HOST_GROW success=%d base_moved=%d\n", result, before != after);
    free(blocker);
    return result;
}

static NativeSymbol symbols[] = {
    {"host_grow", (void *)host_grow, "(i)i", NULL}
};

uint32_t get_native_lib(char **module, NativeSymbol **entries)
{
    *module = "env";
    *entries = symbols;
    return sizeof(symbols) / sizeof(symbols[0]);
}
