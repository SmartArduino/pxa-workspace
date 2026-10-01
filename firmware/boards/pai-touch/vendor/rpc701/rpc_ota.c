#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "rpc_701_internal.h"

static const char *TAG = "rpc_ota";

#define ADD_RPC_BUFF_TO_FREE_LATER(BuFf) {                                    \
    assert((app_req->n_rpc_free_buff_hdls + 1) <= MAX_FREE_BUFF_HANDLES);     \
    app_req->rpc_free_buff_hdls[app_req->n_rpc_free_buff_hdls++] = BuFf;      \
}

#define RPC_ALLOC_ASSIGN(TyPe, MsG_StRuCt, InItFuNc)                          \
    TyPe *req_payload = (TyPe *)calloc(1, sizeof(TyPe));                      \
    if (!req_payload) {                                                       \
        ESP_LOGE(TAG, "Failed to allocate memory for req->%s", #MsG_StRuCt); \
        *failure_status = RPC_ERR_NO_MEMORY;                                  \
        return FAILURE;                                                       \
    }                                                                         \
    req->MsG_StRuCt = req_payload;                                            \
    InItFuNc(req_payload);                                                    \
    ADD_RPC_BUFF_TO_FREE_LATER((uint8_t *)req_payload)

#define RPC_ERR_IN_RESP(msGparaM)                                             \
    if (rpc_msg->msGparaM->resp) {                                            \
        app_resp->resp_event_status = rpc_msg->msGparaM->resp;                \
        ESP_LOGW(TAG,                                                        \
                 "RPC OTA resp [0x%" PRIx16 "], uid [%" PRIu32 "] failed: %" PRIi32, \
                 app_resp->msg_id,                                           \
                 app_resp->uid,                                              \
                 app_resp->resp_event_status);                               \
        goto fail_parse_rpc_msg;                                              \
    }

#define RPC_SEND_REQ(msGiD) do {                                              \
    assert(req);                                                              \
    req->msg_id = msGiD;                                                      \
    if (SUCCESS != rpc_send_req(req)) {                                       \
        ESP_LOGE(TAG, "Failed to send OTA req 0x%x", req->msg_id);           \
        return NULL;                                                          \
    }                                                                         \
} while (0)

#define RPC_DECODE_RSP_IF_NOT_ASYNC() do {                                    \
    if (req->rpc_rsp_cb) {                                                    \
        return NULL;                                                          \
    }                                                                         \
    return rpc_wait_and_parse_sync_resp(req);                                 \
} while (0)

#define CLEANUP_RPC(msg) do {                                                 \
    if (msg) {                                                                \
        if (msg->app_free_buff_hdl && msg->app_free_buff_func) {              \
            msg->app_free_buff_func(msg->app_free_buff_hdl);                  \
            msg->app_free_buff_hdl = NULL;                                    \
        }                                                                     \
        HOSTED_FREE(msg);                                                     \
        msg = NULL;                                                           \
    }                                                                         \
} while (0)

static ctrl_cmd_t *rpc_ota_default_req(void)
{
    ctrl_cmd_t *new_req = (ctrl_cmd_t *)calloc(1, sizeof(ctrl_cmd_t));
    assert(new_req);
    new_req->msg_type = RPC_TYPE__Req;
    new_req->rpc_rsp_cb = NULL;
    new_req->rsp_timeout_sec = DEFAULT_RPC_RSP_TIMEOUT_SEC;
    return new_req;
}

static int rpc_ota_finish_resp(ctrl_cmd_t *app_resp)
{
    int response = RPC_ERR_FAILURE;

    if (!app_resp || app_resp->msg_type != RPC_TYPE__Resp) {
        if (app_resp) {
            ESP_LOGE(TAG, "Recvd Msg[0x%x] is not response", app_resp->msg_type);
        }
        goto done;
    }

    if ((app_resp->msg_id < RPC_ID__Resp_Base) || (app_resp->msg_id >= RPC_ID__Resp_Max)) {
        ESP_LOGE(TAG, "Response Msg ID[0x%x] is not correct", app_resp->msg_id);
        goto done;
    }

    response = app_resp->resp_event_status;

done:
    CLEANUP_RPC(app_resp);
    return response;
}

static int rpc_ota_validate_ptrs(const void *req, void *resp)
{
    if (!req || !resp) {
        ESP_LOGE(TAG, "Invalid OTA req or resp parameter");
        return RPC_ERR_INVALID_PARAM;
    }
    return RPC_ERR_SUCCESS;
}

int rpc_compose_ota_req(Rpc *req, ctrl_cmd_t *app_req, int32_t *failure_status)
{
    switch (req->msg_id) {
    case RPC_ID__Req_OtaBegin: {
        ota_begin_req_t *begin_req = &app_req->u.ota_begin_req;
        RPC_ALLOC_ASSIGN(RpcReqOtaBegin, req_ota_begin, rpc__req__ota_begin__init);
        req->req_ota_begin->session_id = begin_req->session_id;
        req->req_ota_begin->image_size = begin_req->image_size;
        req->req_ota_begin->image_crc32 = begin_req->image_crc32;
        req->req_ota_begin->chunk_size = begin_req->chunk_size;
        req->req_ota_begin->protocol_version = begin_req->protocol_version;
        req->req_ota_begin->target_policy = (OtaTargetPolicy)begin_req->target_policy;
        req->req_ota_begin->transport_hint = (OtaTransportId)begin_req->transport_hint;
        if (begin_req->resume_token && begin_req->resume_token_len > 0) {
            req->req_ota_begin->resume_token.data = (uint8_t *)begin_req->resume_token;
            req->req_ota_begin->resume_token.len = begin_req->resume_token_len;
        }
        break;
    }
    case RPC_ID__Req_OtaChunk: {
        ota_chunk_req_t *chunk_req = &app_req->u.ota_chunk_req;
        RPC_ALLOC_ASSIGN(RpcReqOtaChunk, req_ota_chunk, rpc__req__ota_chunk__init);
        req->req_ota_chunk->session_id = chunk_req->session_id;
        req->req_ota_chunk->seq = chunk_req->seq;
        req->req_ota_chunk->offset = chunk_req->offset;
        if (chunk_req->payload && chunk_req->payload_len > 0) {
            req->req_ota_chunk->payload.data = (uint8_t *)chunk_req->payload;
            req->req_ota_chunk->payload.len = chunk_req->payload_len;
        }
        req->req_ota_chunk->payload_crc32 = chunk_req->payload_crc32;
        break;
    }
    case RPC_ID__Req_OtaCommit: {
        ota_commit_req_t *commit_req = &app_req->u.ota_commit_req;
        RPC_ALLOC_ASSIGN(RpcReqOtaCommit, req_ota_commit, rpc__req__ota_commit__init);
        req->req_ota_commit->session_id = commit_req->session_id;
        req->req_ota_commit->total_size = commit_req->total_size;
        req->req_ota_commit->total_chunks = commit_req->total_chunks;
        req->req_ota_commit->final_crc32 = commit_req->final_crc32;
        break;
    }
    case RPC_ID__Req_OtaAbort: {
        ota_abort_req_t *abort_req = &app_req->u.ota_abort_req;
        RPC_ALLOC_ASSIGN(RpcReqOtaAbort, req_ota_abort, rpc__req__ota_abort__init);
        req->req_ota_abort->session_id = abort_req->session_id;
        req->req_ota_abort->reason_code = (OtaReasonCode)abort_req->reason_code;
        break;
    }
    case RPC_ID__Req_OtaStatusQuery: {
        ota_status_query_req_t *status_req = &app_req->u.ota_status_query_req;
        RPC_ALLOC_ASSIGN(RpcReqOtaStatusQuery, req_ota_status_query,
                         rpc__req__ota_status_query__init);
        req->req_ota_status_query->session_id = status_req->session_id;
        break;
    }
    default:
        *failure_status = RPC_ERR_UNSUPPORTED_MSG;
        ESP_LOGE(TAG, "Unsupported OTA req[%u]", req->msg_id);
        return FAILURE;
    }

    return SUCCESS;
}

int rpc_parse_ota_rsp(Rpc *rpc_msg, ctrl_cmd_t *app_resp)
{
    if (!rpc_msg || !app_resp) {
        ESP_LOGE(TAG, "NULL rpc resp or NULL App Resp");
        goto fail_parse_rpc_msg;
    }

    app_resp->msg_type = RPC_TYPE__Resp;
    app_resp->msg_id = rpc_msg->msg_id;
    app_resp->uid = rpc_msg->uid;

    switch (rpc_msg->msg_id) {
    case RPC_ID__Resp_OtaBegin:
        RPC_FAIL_ON_NULL(resp_ota_begin);
        app_resp->u.ota_begin_resp.state = (ota_session_state_t)rpc_msg->resp_ota_begin->state;
        app_resp->u.ota_begin_resp.reason_code = (ota_reason_code_t)rpc_msg->resp_ota_begin->reason_code;
        app_resp->u.ota_begin_resp.accepted_chunk_size = rpc_msg->resp_ota_begin->accepted_chunk_size;
        app_resp->u.ota_begin_resp.next_offset = rpc_msg->resp_ota_begin->next_offset;
        app_resp->u.ota_begin_resp.transport_id = (ota_transport_id_t)rpc_msg->resp_ota_begin->transport_id;
        app_resp->u.ota_begin_resp.resume_supported = rpc_msg->resp_ota_begin->resume_supported;
        RPC_ERR_IN_RESP(resp_ota_begin);
        break;
    case RPC_ID__Resp_OtaChunk:
        RPC_FAIL_ON_NULL(resp_ota_chunk);
        app_resp->u.ota_chunk_resp.state = (ota_session_state_t)rpc_msg->resp_ota_chunk->state;
        app_resp->u.ota_chunk_resp.reason_code = (ota_reason_code_t)rpc_msg->resp_ota_chunk->reason_code;
        app_resp->u.ota_chunk_resp.accepted_seq = rpc_msg->resp_ota_chunk->accepted_seq;
        app_resp->u.ota_chunk_resp.accepted_offset = rpc_msg->resp_ota_chunk->accepted_offset;
        app_resp->u.ota_chunk_resp.next_offset = rpc_msg->resp_ota_chunk->next_offset;
        RPC_ERR_IN_RESP(resp_ota_chunk);
        break;
    case RPC_ID__Resp_OtaCommit:
        RPC_FAIL_ON_NULL(resp_ota_commit);
        app_resp->u.ota_commit_resp.state = (ota_session_state_t)rpc_msg->resp_ota_commit->state;
        app_resp->u.ota_commit_resp.reason_code = (ota_reason_code_t)rpc_msg->resp_ota_commit->reason_code;
        app_resp->u.ota_commit_resp.received_size = rpc_msg->resp_ota_commit->received_size;
        RPC_ERR_IN_RESP(resp_ota_commit);
        break;
    case RPC_ID__Resp_OtaAbort:
        RPC_FAIL_ON_NULL(resp_ota_abort);
        app_resp->u.ota_abort_resp.state = (ota_session_state_t)rpc_msg->resp_ota_abort->state;
        app_resp->u.ota_abort_resp.reason_code = (ota_reason_code_t)rpc_msg->resp_ota_abort->reason_code;
        RPC_ERR_IN_RESP(resp_ota_abort);
        break;
    case RPC_ID__Resp_OtaStatusQuery:
        RPC_FAIL_ON_NULL(resp_ota_status_query);
        app_resp->u.ota_status_query_resp.state =
            (ota_session_state_t)rpc_msg->resp_ota_status_query->state;
        app_resp->u.ota_status_query_resp.reason_code =
            (ota_reason_code_t)rpc_msg->resp_ota_status_query->reason_code;
        app_resp->u.ota_status_query_resp.received_size =
            rpc_msg->resp_ota_status_query->received_size;
        app_resp->u.ota_status_query_resp.next_offset =
            rpc_msg->resp_ota_status_query->next_offset;
        app_resp->u.ota_status_query_resp.next_seq =
            rpc_msg->resp_ota_status_query->next_seq;
        app_resp->u.ota_status_query_resp.chunk_size =
            rpc_msg->resp_ota_status_query->chunk_size;
        app_resp->u.ota_status_query_resp.image_size =
            rpc_msg->resp_ota_status_query->image_size;
        app_resp->u.ota_status_query_resp.image_crc32 =
            rpc_msg->resp_ota_status_query->image_crc32;
        RPC_ERR_IN_RESP(resp_ota_status_query);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported OTA resp[%u]", rpc_msg->msg_id);
        goto fail_parse_rpc_msg;
    }

    app_resp->resp_event_status = SUCCESS;
    return SUCCESS;

fail_parse_rpc_msg:
    return SUCCESS;
}

ctrl_cmd_t *rpc_slaveif_ota_begin(ctrl_cmd_t *req)
{
    RPC_SEND_REQ(RPC_ID__Req_OtaBegin);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_ota_chunk(ctrl_cmd_t *req)
{
    RPC_SEND_REQ(RPC_ID__Req_OtaChunk);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_ota_commit(ctrl_cmd_t *req)
{
    RPC_SEND_REQ(RPC_ID__Req_OtaCommit);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_ota_abort(ctrl_cmd_t *req)
{
    RPC_SEND_REQ(RPC_ID__Req_OtaAbort);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t *rpc_slaveif_ota_status_query(ctrl_cmd_t *req)
{
    RPC_SEND_REQ(RPC_ID__Req_OtaStatusQuery);
    RPC_DECODE_RSP_IF_NOT_ASYNC();
}

int rpc_ota_begin(const ota_begin_req_t *req_conf, ota_begin_resp_t *resp_out)
{
    ctrl_cmd_t *req = NULL;
    ctrl_cmd_t *resp = NULL;
    int result = rpc_ota_validate_ptrs(req_conf, resp_out);
    if (result != RPC_ERR_SUCCESS) {
        return result;
    }

    req = rpc_ota_default_req();
    req->u.ota_begin_req = *req_conf;
    resp = rpc_slaveif_ota_begin(req);
    if (!resp) {
        return RPC_ERR_FAILURE;
    }

    *resp_out = resp->u.ota_begin_resp;
    return rpc_ota_finish_resp(resp);
}

int rpc_ota_chunk(const ota_chunk_req_t *req_conf, ota_chunk_resp_t *resp_out)
{
    ctrl_cmd_t *req = NULL;
    ctrl_cmd_t *resp = NULL;
    int result = rpc_ota_validate_ptrs(req_conf, resp_out);
    if (result != RPC_ERR_SUCCESS) {
        return result;
    }

    req = rpc_ota_default_req();
    req->u.ota_chunk_req = *req_conf;
    resp = rpc_slaveif_ota_chunk(req);
    if (!resp) {
        return RPC_ERR_FAILURE;
    }

    *resp_out = resp->u.ota_chunk_resp;
    return rpc_ota_finish_resp(resp);
}

int rpc_ota_commit(const ota_commit_req_t *req_conf, ota_commit_resp_t *resp_out)
{
    ctrl_cmd_t *req = NULL;
    ctrl_cmd_t *resp = NULL;
    int result = rpc_ota_validate_ptrs(req_conf, resp_out);
    if (result != RPC_ERR_SUCCESS) {
        return result;
    }

    req = rpc_ota_default_req();
    req->rsp_timeout_sec = 30;
    req->u.ota_commit_req = *req_conf;
    resp = rpc_slaveif_ota_commit(req);
    if (!resp) {
        return RPC_ERR_FAILURE;
    }

    *resp_out = resp->u.ota_commit_resp;
    return rpc_ota_finish_resp(resp);
}

int rpc_ota_abort(const ota_abort_req_t *req_conf, ota_abort_resp_t *resp_out)
{
    ctrl_cmd_t *req = NULL;
    ctrl_cmd_t *resp = NULL;
    int result = rpc_ota_validate_ptrs(req_conf, resp_out);
    if (result != RPC_ERR_SUCCESS) {
        return result;
    }

    req = rpc_ota_default_req();
    req->u.ota_abort_req = *req_conf;
    resp = rpc_slaveif_ota_abort(req);
    if (!resp) {
        return RPC_ERR_FAILURE;
    }

    *resp_out = resp->u.ota_abort_resp;
    return rpc_ota_finish_resp(resp);
}

int rpc_ota_status_query(const ota_status_query_req_t *req_conf, ota_status_query_resp_t *resp_out)
{
    ctrl_cmd_t *req = NULL;
    ctrl_cmd_t *resp = NULL;
    int result = rpc_ota_validate_ptrs(req_conf, resp_out);
    if (result != RPC_ERR_SUCCESS) {
        return result;
    }

    req = rpc_ota_default_req();
    req->u.ota_status_query_req = *req_conf;
    resp = rpc_slaveif_ota_status_query(req);
    if (!resp) {
        return RPC_ERR_FAILURE;
    }

    *resp_out = resp->u.ota_status_query_resp;
    return rpc_ota_finish_resp(resp);
}
