/* Run a headless WASI regression with one explicitly mounted data directory.
 * Host execution time is not a target-device performance measurement. */
#include "wasm_export.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s module.aot data-directory program guest-args...\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 2;
    long length = ftell(file);
    if (length < 1 || (uint64_t)length > UINT32_MAX) { fclose(file); return 2; }
    rewind(file);
    uint8_t *bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(bytes); return 2;
    }
    fclose(file);
    if (!wasm_runtime_init()) { free(bytes); return 2; }
    char error[256];
    wasm_module_t module = wasm_runtime_load(bytes, (uint32_t)length, error, sizeof(error));
    int result = 1;
    if (!module) { fprintf(stderr, "load: %s\n", error); goto done; }
    size_t map_length = strlen(argv[2]) + sizeof("/roms::");
    char *mapping = malloc(map_length);
    if (!mapping) goto unload;
    snprintf(mapping, map_length, "/roms::%s", argv[2]);
    const char *maps[] = {mapping};
    wasm_runtime_set_wasi_args(module, NULL, 0, maps, 1, NULL, 0, argv + 3, argc - 3);
    wasm_module_inst_t instance = wasm_runtime_instantiate(module, 262144, 0, error, sizeof(error));
    if (!instance) { fprintf(stderr, "instantiate: %s\n", error); free(mapping); goto unload; }
    if (!wasm_application_execute_main(instance, 0, NULL))
        fprintf(stderr, "execute: %s\n", wasm_runtime_get_exception(instance));
    else
        result = wasm_runtime_get_wasi_exit_code(instance) ? 1 : 0;
    wasm_runtime_deinstantiate(instance);
    free(mapping);
unload:
    wasm_runtime_unload(module);
done:
    wasm_runtime_destroy(); free(bytes);
    return result;
}
