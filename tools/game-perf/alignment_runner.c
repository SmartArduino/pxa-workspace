#include "wasm_export.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static wasm_module_inst_t instance;
static wasm_exec_env_t env;
static uint32_t call(const char *name, uint32_t a, uint32_t b, unsigned n, int pass) {
    uint32_t args[2] = {a, b};
    wasm_function_inst_t fn = wasm_runtime_lookup_function(instance, name);
    assert(fn);
    int ok = wasm_runtime_call_wasm(env, fn, n, args);
    if (!pass) {
        assert(!ok);
        const char *e = wasm_runtime_get_exception(instance);
        assert(e && strstr(e, "out of bounds"));
        wasm_runtime_clear_exception(instance);
        return 0;
    }
    if (!ok) {
        fprintf(stderr, "%s\n", wasm_runtime_get_exception(instance));
        abort();
    }
    return args[0];
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *fp = fopen(argv[1], "rb");
    assert(fp);
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    uint8_t *buf = malloc(len);
    assert(fread(buf, 1, len, fp) == len);
    fclose(fp);
    assert(wasm_runtime_init());
    char error[256];
    wasm_module_t m = wasm_runtime_load(buf, len, error, sizeof(error));
    if (!m) {
        fprintf(stderr, "load: %s\n", error);
        return 1;
    }
    instance = wasm_runtime_instantiate(m, 65536, 0, error, sizeof(error));
    if (!instance) {
        fprintf(stderr, "instantiate: %s\n", error);
        return 1;
    }
    env = wasm_runtime_create_exec_env(instance, 65536);
    assert(env);
    for (unsigned i = 0; i < 128; i++)
        call("store_byte", 65536 + i, (i * 17 + 3) & 255, 2, 1);
    for (unsigned a = 65536; a < 65632; a++) {
        uint32_t expected = 0;
        for (unsigned j = 0; j < 4; j++)
            expected |= (((a - 65536 + j) * 17 + 3) & 255) << (8 * j);
        assert(call("load_any", a, 0, 1, 1) == expected);
        if ((a & 3) == 0)
            assert(call("load_aligned", a + 3, 0, 1, 1) == expected);
        if ((a & 7) == 4)
            assert(call("load_offset", a - 4, 0, 1, 1) == expected);
    }
    for (unsigned i = 0; i < 4; i++) {
        call("store_stride", 65536 + i, 8, 2, 1);
        assert(call("load_stride", 65536 + i, 8, 2, 1) == 28u * 257u + 24u);
        for (unsigned j = 0; j < 8; j++)
            assert(call("load_any", 65536 + j * 4, 0, 1, 1) == j * 257u + 3u);
    }
    for (unsigned i = 0; i < 4; i++) {
        call("store_any", 65536 + i, 0xa1b2c3d4u + i, 2, 1);
        assert(call("load_any", 65536 + i, 0, 1, 1) == 0xa1b2c3d4u + i);
    }
    call("store_aligned", 65539, 0xaabbccddu, 2, 1);
    assert(call("load_aligned", 65539, 0, 1, 1) == 0xaabbccddu);
    assert(call("load_half", 65539, 0, 1, 1) == 0xaabbu);
    call("store_any", 131068, 0x12345678, 2, 1);
    assert(call("load_any", 131068, 0, 1, 1) == 0x12345678);
    call("load_any", 131069, 0, 1, 0);
    call("store_any", 131069, 42, 2, 0);
    call("load_aligned", 131072, 0, 1, 0);
    call("store_aligned", 131072, 42, 2, 0);
    call("load_any", UINT32_MAX, 0, 1, 0);
    call("store_stride", 131068, 1, 2, 1);
    assert(call("load_stride", 131068, 1, 2, 1) == 3);
    call("load_stride", 131068, 2, 2, 0);
    call("store_stride", 131068, 2, 2, 0);
    assert(call("load_stride", UINT32_MAX, 0, 2, 1) == 0);
    call("store_stride", UINT32_MAX, 0, 2, 1);
    /* Access after memory.grow must reload both the base and the limit. */
    assert(call("grow_then_load", 131072, 1, 2, 1) == 0);
    call("store_any", 196604, 0x87654321u, 2, 1);
    assert(call("load_any", 196604, 0, 1, 1) == 0x87654321u);
    assert(call("grow_then_load", 196604, 1, 2, 1) == 0x87654321u);
    call("load_any", 196605, 0, 1, 0);
    call("store_any", 196605, 42, 2, 0);
    call("grow_then_load", 196608, 1, 2, 0);
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(m);
    wasm_runtime_destroy();
    free(buf);
    puts("PASS: aligned and all unaligned offsets, end-of-memory access, OOB loads/stores and "
         "overflow remain correct");
    return 0;
}
