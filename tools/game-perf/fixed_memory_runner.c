#include "wasm_export.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static wasm_module_inst_t instance;
static wasm_exec_env_t env;
static uint32_t mutate(wasm_exec_env_t exec, uint32_t address, uint32_t value,
                       uint32_t extra_pages) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec);
    assert(wasm_runtime_validate_app_addr(inst, address, 4));
    memcpy(wasm_runtime_addr_app_to_native(inst, address), &value, 4);
    uint32_t old = wasm_memory_get_cur_page_count(wasm_runtime_get_default_memory(inst));
    return wasm_runtime_enlarge_memory(inst, extra_pages) ? old : UINT32_MAX;
}
static uint32_t call(const char *name, uint32_t a, uint32_t b, uint32_t c,
                     unsigned count, int pass) {
    uint32_t args[3] = {a, b, c};
    wasm_function_inst_t fn = wasm_runtime_lookup_function(instance, name);
    assert(fn);
    int ok = wasm_runtime_call_wasm(env, fn, count, args);
    if (!pass) {
        assert(!ok);
        const char *e = wasm_runtime_get_exception(instance);
        assert(e && strstr(e, "out of bounds"));
        wasm_runtime_clear_exception(instance);
    } else if (!ok) {
        fprintf(stderr, "%s: %s\n", name, wasm_runtime_get_exception(instance));
        abort();
    }
    return args[0];
}
static uint32_t result(void) { return call("growth_result", 0, 0, 0, 0, 1); }
int main(int argc, char **argv) {
    assert(argc == 4);
    unsigned heap = strtoul(argv[2], NULL, 0);
    int fixed = atoi(argv[3]);
    FILE *fp = fopen(argv[1], "rb"); assert(fp);
    fseek(fp, 0, SEEK_END); long len = ftell(fp); rewind(fp);
    uint8_t *buf = malloc(len); assert(buf);
    assert(fread(buf, 1, len, fp) == (size_t)len); fclose(fp);
    assert(wasm_runtime_init());
    NativeSymbol symbols[] = {{"mutate", (void *)mutate, "(iii)i", NULL}};
    assert(wasm_runtime_register_natives("env", symbols, 1));
    char error[256];
    wasm_module_t module = wasm_runtime_load(buf, len, error, sizeof(error));
    if (!module) { fprintf(stderr, "load: %s\n", error); return 1; }
    instance = wasm_runtime_instantiate(module, 65536, heap, error, sizeof(error));
    if (!instance) { fprintf(stderr, "instantiate: %s\n", error); return 1; }
    env = wasm_runtime_create_exec_env(instance, 65536); assert(env);
    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(instance);
    uint32_t pages = wasm_memory_get_cur_page_count(mem);
    uint32_t limit = pages * wasm_memory_get_bytes_per_page(mem);
    if (fixed == 2) {
        assert(limit > 0 && limit < 32768);
        call("read_word", limit - 4, 0, 0, 1, 1);
        call("read_word", limit - 3, 0, 0, 1, 0);
        call("write_word", limit - 3, 42, 0, 2, 0);
        call("bounded_read", 0, 0, 0, 1, 0);
        call("bounded_write", 0, 42, 0, 2, 0);
        goto done;
    }
    assert(call("pages", 0, 0, 0, 0, 1) == pages);
    assert((pages == wasm_memory_get_max_page_count(mem)) == fixed);
    /* Memory contents must never be cached across an imported call, even when
     * its base and bounds are fixed. Include all byte alignments. */
    for (uint32_t i = 0; i < 4; i++) {
        call("write_word", 32768 + i, 0x12345678, 0, 2, 1);
        assert(call("call_then_read", 32768 + i, 0x89abcdef, 0, 3, 1) ==
               (0x12345678u ^ 0x89abcdefu));
        assert(result() == pages);
        assert(call("loop_calls", 32768 + i, 100, 0, 2, 1) == 4950);
    }
    for (uint32_t i = 0; i < 256; ++i) {
        call("bounded_write", i, i * 17, 0, 2, 1);
        assert(call("bounded_read", i + 256, 0, 0, 1, 1) == i * 17);
    }
    assert(call("bounded_read", UINT32_MAX, 0, 0, 1, 1) == 255 * 17);
    /* Use runtime bounds: host-managed heap insertion can enlarge them, even
     * without changing memory.size in the fixed one-page special case. Do
     * not overwrite the allocator's own metadata at the end of that heap. */
    uint32_t end_word = call("read_word", limit - 4, 0, 0, 1, 1);
    assert(call("grow_then_read", limit - 4, 0, 0, 2, 1) == end_word);
    assert(result() == pages);
    call("read_word", limit - 3, 0, 0, 1, 0);
    call("write_word", limit - 3, 42, 0, 2, 0);
    call("read_word", UINT32_MAX, 0, 0, 1, 0);
    call("write_word", UINT32_MAX, 42, 0, 2, 0);
    if (fixed) {
        assert(call("grow_then_read", limit - 4, 1, 0, 2, 1) == end_word);
        assert(result() == UINT32_MAX);
        assert(call("call_then_read_end", limit - 4, 1, 0, 2, 1) == end_word);
        assert(result() == UINT32_MAX);
        call("call_then_read_end", limit, 1, 0, 2, 0);
        call("grow_then_read", limit, 1, 0, 2, 0);
    } else {
        /* Successful host growth inside the same guest frame must refresh
         * base and bounds. The newly allocated page is zero-filled. */
        assert(call("call_then_read_end", limit, 1, 0, 2, 1) == 0);
        assert(result() == pages);
        limit += wasm_memory_get_bytes_per_page(mem);
        assert(call("grow_then_read", limit - 4, 1, 0, 2, 1) == 0);
        assert(result() == UINT32_MAX);
        call("grow_then_read", limit, 0, 0, 2, 0);
    }
done:
    printf("PASS fixed=%d heap=%u pages=%u bytes=%u: memory bounds\n",
           fixed, heap, pages, limit);
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
    wasm_runtime_destroy();
    free(buf);
    return 0;
}
