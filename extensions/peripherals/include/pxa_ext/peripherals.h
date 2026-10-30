// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_EXT_PERIPHERALS_H
#define PXA_EXT_PERIPHERALS_H
#include <pxa/permission.h>
#include <stddef.h>
#include "peripherals_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint16_t id, kind;
    uint32_t flags;
    char name[PXA_PERIPHERALS_NAME_BYTES];
} pxa_peripheral_port_t;
typedef struct {
    uint32_t generation, flags; /* present=1, valid=2, claimed=4, unknown=8, error=16 */
    uint16_t board_type, board_id, hardware_version, vendor_id;
    uint32_t serial_number;
    char name[32];
} pxa_peripheral_module_t;
typedef struct {
    uint32_t struct_size;
    void *context;
    pxa_status_t (*list)(void *, pxa_peripheral_port_t *, size_t capacity, size_t *count);
    pxa_status_t (*module)(void *, uint16_t port, pxa_peripheral_module_t *);
    /* Any non-NULL device returned by open is owned by the Host, including
     * partial creation on error; the Host calls close to roll it back. */
    pxa_status_t (*open)(void *, uint64_t owner, uint16_t port, uint16_t mode,
        uint32_t parameter, uint16_t address, void **device);
    int32_t (*io)(void *, void *device, uint32_t operation, uint8_t *, size_t);
    /* start copies input before returning; poll never performs device I/O.
     * Cookies distinguish queued work even after a resource slot is reused. */
    pxa_status_t (*start)(void *, void *device, uint64_t cookie,
        const uint8_t *, uint16_t tx_size, uint16_t rx_size);
    /* On success bytes must equal the accepted rx_size, including zero for write-only I/O. */
    pxa_status_t (*poll)(void *, void *device, uint64_t cookie, uint8_t *, size_t *bytes);
    void (*cancel)(void *, void *device, uint64_t cookie);
    /* Nonblocking: retain native resources until any worker finishes. */
    void (*close)(void *, void *device);
    /* Optional asynchronous initialization. open only reserves/queues a device;
     * ready returns WOULD_BLOCK until prepared, OK or a known error. It must
     * never do device I/O. close cancels preparation and retains native state
     * until the worker has finished. Omit for synchronous backends. */
    pxa_status_t (*ready)(void *, void *device);
} pxa_peripherals_backend_t;
#define PXA_PERIPHERALS_BACKEND_BASE_SIZE offsetof(pxa_peripherals_backend_t, ready)
typedef struct pxa_peripherals_service pxa_peripherals_service_t;
size_t pxa_peripherals_service_workspace_size(void);
pxa_status_t pxa_peripherals_service_init(void *, size_t, pxa_runtime_t *,
    pxa_permission_service_t *, const pxa_peripherals_backend_t *, pxa_peripherals_service_t **);
pxa_status_t pxa_peripherals_service_register(pxa_peripherals_service_t *);
void pxa_peripherals_service_set_foreground(pxa_peripherals_service_t *, pxa_component_t);
int pxa_peripherals_service_pending(const pxa_peripherals_service_t *);
void pxa_peripherals_service_poll(pxa_peripherals_service_t *);
#ifdef __cplusplus
}
#endif
#endif
