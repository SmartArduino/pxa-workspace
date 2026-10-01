#ifndef RPC_OTA_H
#define RPC_OTA_H

#include "rpc_701.h"

#ifdef __cplusplus
extern "C" {
#endif

int rpc_compose_ota_req(Rpc *req, ctrl_cmd_t *app_req, int32_t *failure_status);
int rpc_parse_ota_rsp(Rpc *rpc_msg, ctrl_cmd_t *app_resp);

#ifdef __cplusplus
}
#endif

#endif /* RPC_OTA_H */