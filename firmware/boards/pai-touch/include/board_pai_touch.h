#ifndef BOARD_PAI_TOUCH_H
#define BOARD_PAI_TOUCH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the pai-touch implementation of the generic board port. */
bool board_pai_touch_register(void);

#ifdef __cplusplus
}
#endif

#endif
