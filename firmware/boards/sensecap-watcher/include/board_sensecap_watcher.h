#ifndef BOARD_SENSECAP_WATCHER_H
#define BOARD_SENSECAP_WATCHER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the SenseCAP Watcher implementation of the generic board port. */
bool board_sensecap_watcher_register(void);

#ifdef __cplusplus
}
#endif

#endif
