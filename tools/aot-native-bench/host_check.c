#include "kernels.h"
#include "wasm_export.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ITEM(name) {#name, name},
static const struct { const char *name; bench_kernel_t function; } kernels[] = {
    BENCH_KERNELS(ITEM)
};
#undef ITEM
static void fill(uint8_t *data) {
    uint32_t x = 0x12345678U;
    for (uint32_t i = 0; i < BENCH_BYTES; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        data[i] = (uint8_t)x;
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *fp = fopen(argv[1], "rb"); assert(fp);
    fseek(fp, 0, SEEK_END); long bytes = ftell(fp); rewind(fp);
    uint8_t *binary = malloc(bytes); assert(binary);
    assert(fread(binary, 1, bytes, fp) == (size_t)bytes); fclose(fp);
    assert(wasm_runtime_init());
    char error[256];
    wasm_module_t module = wasm_runtime_load(binary, bytes, error, sizeof(error));
    if (!module) { fprintf(stderr, "%s\n", error); return 1; }
    wasm_module_inst_t instance = wasm_runtime_instantiate(module, 32768, 0, error, sizeof(error));
    if (!instance) { fprintf(stderr, "%s\n", error); return 1; }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(instance, 32768); assert(env);
    assert(wasm_runtime_validate_app_addr(instance, BENCH_OFFSET, BENCH_BYTES));
    uint8_t *data = wasm_runtime_addr_app_to_native(instance, BENCH_OFFSET);
    uint8_t *expected = malloc(BENCH_BYTES); assert(expected);
    for (unsigned k = 0; k < sizeof(kernels)/sizeof(kernels[0]); ++k) {
        for (unsigned n = 1; n <= 65536; n *= 16) {
            fill(expected); fill(data);
            uint32_t value = kernels[k].function(expected, n, 0x31415926U);
            uint32_t args[] = {BENCH_OFFSET, n, 0x31415926U};
            wasm_function_inst_t fn = wasm_runtime_lookup_function(instance, kernels[k].name); assert(fn);
            if (!wasm_runtime_call_wasm(env, fn, 3, args)) {
                fprintf(stderr, "%s\n", wasm_runtime_get_exception(instance)); return 1;
            }
            assert(value == args[0]); assert(memcmp(data, expected, BENCH_BYTES) == 0);
        }
        printf("PASS %s: return value and every output byte\n", kernels[k].name);
    }
    free(expected); wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(instance); wasm_runtime_unload(module);
    wasm_runtime_destroy(); free(binary);
    return 0;
}
