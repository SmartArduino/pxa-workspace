/* Test-only linker wrapper. Never include this file in release firmware.
 * Compile with the pxa_wamr_aot_reader.c include/target flags and link using
 * -Wl,--wrap=pxa_wamr_read_aot. Xtensa also requires -mlongcalls.
 * The marker must be on the test device's already mounted PXA filesystem.
 */
#include "pxa_wamr_aot_reader.h"
#include <fcntl.h>
#include <unistd.h>

#ifndef PXA_AOT_REFERENCE_FLAG
#define PXA_AOT_REFERENCE_FLAG "/assets/pxa-state/inbox/aot-buffered-reference.flag"
#endif

pxa_status_t __real_pxa_wamr_read_aot(pxa_wamr_artifact_reader_t *,
                                    wasm_module_t *, int *, char *, uint32_t);

pxa_status_t __wrap_pxa_wamr_read_aot(pxa_wamr_artifact_reader_t *reader,
                                    wasm_module_t *module, int *buffered,
                                    char *error, uint32_t error_size) {
    int fd = open(PXA_AOT_REFERENCE_FLAG, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        *module = NULL;
        *buffered = 1;
        return PXA_STATUS_OK;
    }
    return __real_pxa_wamr_read_aot(reader, module, buffered, error, error_size);
}
