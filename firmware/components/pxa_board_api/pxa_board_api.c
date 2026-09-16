#include "pxa_board_api.h"

static const pxa_board_port_t* g_board_port;

bool pxa_board_register(const pxa_board_port_t* port) {
    if (port == NULL || port->struct_size != sizeof(*port) ||
        port->initialize == NULL || port->display == NULL ||
        port->display_profile == NULL) {
        return false;
    }
    if (g_board_port != NULL && g_board_port != port)
        return false;
    g_board_port = port;
    return true;
}

const pxa_board_port_t* pxa_board_current(void) {
    return g_board_port;
}
