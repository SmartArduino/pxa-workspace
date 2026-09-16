#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Every selected board exports this stable entry point to the product main.
bool pxa_board_register_selected(void);

#ifdef __cplusplus
}
#endif
