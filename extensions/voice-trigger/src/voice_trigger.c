// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/voice_trigger.h>
#include <string.h>

#define VT_MAGIC UINT32_C(0x56543031)
#define VT_SCRATCH (8u + PXA_VT_MAX_KEYWORDS * (6u + PXA_VT_MAX_WORD_BYTES))
#define VT_MAX_EVENT_AGE_US UINT64_C(2000000)

struct pxa_vt_service {
    uint32_t magic;
    pxa_runtime_t *runtime;
    pxa_permission_service_t *permissions;
    pxa_vt_backend_t backend;
    pxa_component_t foreground, owner;
    pxa_handle64_t handle;
    uint32_t revision;
    uint32_t subscribed_keywords[PXA_VT_MAX_KEYWORDS];
    uint16_t subscribed_count;
    uint64_t last_sequence, last_timestamp;
    uint8_t pending[PXA_VT_EVENT_BYTES];
    uint8_t scratch[VT_SCRATCH];
    pxa_vt_profile_t profile;
    uint8_t registered, detecting, pending_event;
};

static int valid(const pxa_vt_service_t *s) { return s && s->magic == VT_MAGIC; }
static pxa_status_t normalized(pxa_status_t status) {
    return pxa_status_is_known(status) ? status : PXA_STATUS_IO_ERROR;
}

static pxa_status_t refresh(pxa_vt_service_t *s) {
    uint16_t i, j;
    pxa_status_t status;
    memset(&s->profile, 0, sizeof(s->profile));
    status = normalized(s->backend.profile(s->backend.context, &s->profile));
    if (status != PXA_STATUS_OK) return status;
    if (s->profile.ready > 1 || !s->profile.revision ||
        s->profile.keyword_count > PXA_VT_MAX_KEYWORDS ||
        s->profile.update_mode != PXA_VT_FIXED_CATALOG ||
        (s->profile.flags & ~(PXA_VT_FOREGROUND_ONLY | PXA_VT_PLAYBACK_DETECTION)))
        return PXA_STATUS_PROTOCOL_ERROR;
    if (s->profile.ready && !s->profile.keyword_count)
        return PXA_STATUS_PROTOCOL_ERROR;
    for (i = 0; i < s->profile.keyword_count; ++i) {
        const pxa_vt_keyword_t *word = &s->profile.keywords[i];
        if (!word->id || !word->label[0] ||
            !memchr(word->label, 0, sizeof(word->label)))
            return PXA_STATUS_PROTOCOL_ERROR;
        if (!pxa_vt_valid_word(word->label, strlen(word->label)))
            return PXA_STATUS_PROTOCOL_ERROR;
        for (j = 0; j < i; ++j)
            if (word->id == s->profile.keywords[j].id ||
                strcmp(word->label, s->profile.keywords[j].label) == 0)
                return PXA_STATUS_PROTOCOL_ERROR;
    }
    s->profile.flags |= PXA_VT_FOREGROUND_ONLY;
    return PXA_STATUS_OK;
}

size_t pxa_vt_service_workspace_size(void) { return sizeof(pxa_vt_service_t); }

pxa_status_t pxa_vt_service_init(void *workspace, size_t size,
    pxa_runtime_t *runtime, pxa_permission_service_t *permissions,
    const pxa_vt_backend_t *backend, pxa_vt_service_t **output) {
    pxa_vt_service_t *s;
    if (!output) return PXA_STATUS_INVALID_ARGUMENT;
    *output = NULL;
    if (!workspace || size < sizeof(*s) ||
        (uintptr_t)workspace % sizeof(uint64_t) || !runtime || !permissions ||
        !backend || backend->struct_size < sizeof(*backend) ||
        !backend->profile || !backend->start || !backend->read || !backend->stop)
        return PXA_STATUS_INVALID_ARGUMENT;
    s = workspace;
    memset(s, 0, sizeof(*s));
    s->magic = VT_MAGIC;
    s->runtime = runtime;
    s->permissions = permissions;
    s->backend = *backend;
    *output = s;
    return PXA_STATUS_OK;
}

static void close_subscription(void *context) {
    pxa_vt_service_t *s = context;
    if (!valid(s)) return;
    if (s->detecting) s->backend.stop(s->backend.context);
    s->handle = 0;
    s->owner = PXA_COMPONENT_INVALID;
    s->detecting = s->pending_event = 0;
    s->subscribed_count = 0;
    s->last_sequence = s->last_timestamp = 0;
}

static int32_t unsupported_io(void *context, uint32_t operation,
                             uint8_t *data, size_t size) {
    (void)context; (void)operation; (void)data; (void)size;
    return PXA_STATUS_UNSUPPORTED;
}
static const pxa_resource_ops_t resource_ops = {sizeof(pxa_resource_ops_t), unsupported_io};

static pxa_status_t subscribe(pxa_vt_service_t *s, pxa_component_t component,
                             pxa_bytes_t payload) {
    static const uint8_t permission[] = PXA_VT_PERMISSION;
    static const uint8_t scope[] = PXA_VT_DEFAULT_SCOPE;
    pxa_authority_t authority;
    pxa_resource_t resource = {s, &resource_ops, close_subscription};
    pxa_status_t status;
    if (payload.size != 16 || !(pxa_read_u64(payload.data + 8) >> 32))
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!s->foreground || component != s->foreground) return PXA_STATUS_DENIED;
    if (pxa_read_u32(payload.data) != PXA_VT_DEFAULT_PROFILE) return PXA_STATUS_NOT_FOUND;
    status = pxa_permission_resolve64(s->permissions, component,
        pxa_read_u64(payload.data + 8),
        (pxa_bytes_t){permission, sizeof(permission)-1},
        (pxa_bytes_t){scope, sizeof(scope)-1}, &authority);
    if (status != PXA_STATUS_OK) return status;
    if (s->handle) return PXA_STATUS_BUSY;
    status = refresh(s);
    if (status != PXA_STATUS_OK) return status;
    if (!s->profile.ready) return PXA_STATUS_UNAVAILABLE;
    if (pxa_read_u32(payload.data + 4) != s->profile.revision)
        return PXA_STATUS_BAD_STATE;
    status = normalized(s->backend.start(s->backend.context, s->profile.revision));
    if (status != PXA_STATUS_OK) return status;
    s->detecting = 1;
    status = pxa_handle_open64(s->runtime, component, PXA_VT_RESOURCE_TYPE,
        authority, &resource, &s->handle);
    if (status != PXA_STATUS_OK) { close_subscription(s); return status; }
    s->owner = component;
    s->revision = s->profile.revision;
    s->subscribed_count = s->profile.keyword_count;
    for (uint16_t i = 0; i < s->subscribed_count; ++i)
        s->subscribed_keywords[i] = s->profile.keywords[i].id;
    return PXA_STATUS_OK;
}

static pxa_status_t command(pxa_vt_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m, size_t *size) {
    static const uint8_t identity[] = PXA_VT_IDENTITY;
    static const char scope[] = PXA_VT_DEFAULT_SCOPE;
    pxa_status_t status;
    uint16_t i;
    uint8_t *out = s->scratch;
    *size = 0;
    if (m->opcode == PXA_VT_HELLO) {
        if (m->payload.size != 24) return PXA_STATUS_INVALID_ARGUMENT;
        if (memcmp(m->payload.data, identity, sizeof(identity)) ||
            pxa_read_u16(m->payload.data + 16) != PXA_VT_MAJOR ||
            pxa_read_u16(m->payload.data + 18) > PXA_VT_MINOR ||
            pxa_read_u16(m->payload.data + 20) != PXA_VT_MAJOR ||
            pxa_read_u16(m->payload.data + 22) < PXA_VT_MINOR)
            return PXA_STATUS_UNSUPPORTED;
        memcpy(out, identity, sizeof(identity));
        pxa_write_u16(out + 16, PXA_VT_MAJOR);
        pxa_write_u16(out + 18, PXA_VT_MINOR);
        *size = 20;
        return PXA_STATUS_OK;
    }
    if (m->opcode == PXA_VT_SUBSCRIBE) {
        status = subscribe(s, component, m->payload);
        if (status != PXA_STATUS_OK) return status;
        pxa_write_u64(out, s->handle);
        pxa_write_u32(out + 8, s->revision);
        *size = 12;
        return PXA_STATUS_OK;
    }
    if ((m->opcode == PXA_VT_CAPABILITIES || m->opcode == PXA_VT_LIST_PROFILES) &&
        m->payload.size) return PXA_STATUS_INVALID_ARGUMENT;
    if (m->opcode == PXA_VT_LIST_KEYWORDS && m->payload.size != 8)
        return PXA_STATUS_INVALID_ARGUMENT;
    status = refresh(s);
    if (status != PXA_STATUS_OK) return status;
    if (m->opcode == PXA_VT_CAPABILITIES) {
        pxa_write_u32(out, s->profile.flags);
        pxa_write_u32(out + 4, s->profile.ready);
        pxa_write_u16(out + 8, 1); /* profiles */
        pxa_write_u16(out + 10, 1); /* simultaneous subscriptions */
        pxa_write_u16(out + 12, PXA_VT_MAX_KEYWORDS);
        pxa_write_u16(out + 14, PXA_VT_MAX_WORD_BYTES);
        pxa_write_u32(out + 16, s->profile.revision);
        pxa_write_u32(out + 20, s->profile.update_mode);
        *size = PXA_VT_CAPABILITIES_BYTES;
    } else if (m->opcode == PXA_VT_LIST_PROFILES) {
        pxa_write_u16(out, 1);
        pxa_write_u32(out + 2, PXA_VT_DEFAULT_PROFILE);
        pxa_write_u32(out + 6, s->profile.revision);
        pxa_write_u16(out + 10, s->profile.keyword_count);
        pxa_write_u16(out + 12, sizeof(scope)-1);
        memcpy(out + 14, scope, sizeof(scope)-1);
        *size = 14 + sizeof(scope)-1;
    } else if (m->opcode == PXA_VT_LIST_KEYWORDS) {
        if (pxa_read_u32(m->payload.data) != PXA_VT_DEFAULT_PROFILE)
            return PXA_STATUS_NOT_FOUND;
        if (pxa_read_u32(m->payload.data + 4) != s->profile.revision)
            return PXA_STATUS_BAD_STATE;
        if (!s->profile.ready) return PXA_STATUS_UNAVAILABLE;
        pxa_write_u32(out, s->profile.revision);
        pxa_write_u16(out + 4, s->profile.keyword_count);
        pxa_write_u16(out + 6, 0);
        *size = 8;
        for (i = 0; i < s->profile.keyword_count; ++i) {
            const pxa_vt_keyword_t *word = &s->profile.keywords[i];
            size_t length = strlen(word->label);
            pxa_write_u32(out + *size, word->id);
            pxa_write_u16(out + *size + 4, (uint16_t)length);
            memcpy(out + *size + 6, word->label, length);
            *size += 6 + length;
        }
    } else return PXA_STATUS_UNSUPPORTED;
    return PXA_STATUS_OK;
}

static pxa_status_t control(void *context, pxa_runtime_t *runtime,
    pxa_component_t component, const pxa_message_view_t *m) {
    pxa_vt_service_t *s = context;
    uint16_t wire_profile;
    size_t result_size = 0;
    pxa_status_t status, complete;
    if (!valid(s) || !m->request_token) return PXA_STATUS_INVALID_ARGUMENT;
    if (m->opcode < PXA_VT_HELLO || m->opcode > PXA_VT_SUBSCRIBE)
        return PXA_STATUS_UNSUPPORTED;
    status = pxa_component_wire_profile(runtime, component, &wire_profile);
    if (status != PXA_STATUS_OK) return status;
    if (wire_profile != 1) return PXA_STATUS_UNSUPPORTED;
    status = pxa_request_begin_reserved(runtime, component, m->request_token,
        PXA_VT_SERVICE_ID, m->opcode, 0,
        m->opcode == PXA_VT_LIST_KEYWORDS ? VT_SCRATCH : 64);
    if (status != PXA_STATUS_OK) return status;
    status = pxa_request_commit(runtime, component, m->request_token);
    if (status == PXA_STATUS_OK) status = command(s, component, m, &result_size);
    complete = pxa_request_complete(runtime, component, m->request_token,
        status, status == PXA_STATUS_OK ? s->scratch : NULL,
        status == PXA_STATUS_OK ? result_size : 0);
    if (complete != PXA_STATUS_OK) {
        if (status == PXA_STATUS_OK && m->opcode == PXA_VT_SUBSCRIBE)
            (void)pxa_handle_close64(runtime, component, s->handle);
        (void)pxa_request_cancel(runtime, component, m->request_token);
    }
    return PXA_STATUS_OK;
}

pxa_status_t pxa_vt_service_register(pxa_vt_service_t *s) {
    pxa_service_ops_t ops;
    pxa_status_t status;
    if (!valid(s)) return PXA_STATUS_INVALID_ARGUMENT;
    if (s->registered) return PXA_STATUS_BAD_STATE;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.service_id = PXA_VT_SERVICE_ID;
    ops.major = PXA_VT_MAJOR;
    ops.minor = PXA_VT_MINOR;
    ops.context = s;
    ops.control = control;
    status = pxa_service_register(s->runtime, &ops);
    if (status == PXA_STATUS_OK) s->registered = 1;
    return status;
}

void pxa_vt_service_set_foreground(pxa_vt_service_t *s, pxa_component_t component) {
    if (!valid(s)) return;
    if (s->handle && s->owner != component)
        (void)pxa_handle_close64(s->runtime, s->owner, s->handle);
    s->foreground = component;
}
int pxa_vt_service_active(const pxa_vt_service_t *s) { return valid(s) && s->handle != 0; }

static void encode_event(pxa_vt_service_t *s, uint32_t kind,
    const pxa_vt_match_t *match, pxa_status_t status) {
    pxa_write_u64(s->pending, s->handle);
    pxa_write_u64(s->pending + 8, match->sequence);
    pxa_write_u64(s->pending + 16, match->timestamp_us);
    pxa_write_u32(s->pending + 24, match->revision);
    pxa_write_u32(s->pending + 28, match->keyword_id);
    pxa_write_u32(s->pending + 32, kind);
    pxa_write_u32(s->pending + 36, (uint32_t)status);
    s->pending_event = 1;
}
static void fault(pxa_vt_service_t *s, uint64_t now, pxa_status_t status) {
    pxa_vt_match_t match = {s->last_sequence, now, s->revision, 0};
    if (s->detecting) s->backend.stop(s->backend.context);
    s->detecting = 0;
    encode_event(s, PXA_VT_FAILED, &match, status);
}

pxa_status_t pxa_vt_service_poll(pxa_vt_service_t *s, uint64_t now) {
    pxa_status_t status;
    unsigned n;
    if (!valid(s)) return PXA_STATUS_INVALID_ARGUMENT;
    if (!s->handle) return PXA_STATUS_OK;
    if (s->owner != s->foreground) {
        pxa_vt_service_set_foreground(s, s->foreground);
        return PXA_STATUS_OK;
    }
    for (n = 0; n < 4; ++n) {
        if (s->pending_event) {
            if (pxa_read_u32(s->pending + 32) == PXA_VT_TRIGGERED &&
                now >= pxa_read_u64(s->pending + 16) &&
                now - pxa_read_u64(s->pending + 16) > VT_MAX_EVENT_AGE_US)
                fault(s, now, PXA_STATUS_TIMED_OUT);
            status = pxa_event_post_message(s->runtime, s->owner,
                PXA_VT_SERVICE_ID, PXA_VT_EVENT, 0,
                (pxa_bytes_t){s->pending, sizeof(s->pending)}, 1, 0);
            if (status != PXA_STATUS_OK) return status;
            s->pending_event = 0;
        }
        if (!s->detecting) return PXA_STATUS_OK;
        {
            pxa_vt_match_t match;
            uint16_t i;
            status = normalized(s->backend.read(s->backend.context, &match));
            if (status == PXA_STATUS_WOULD_BLOCK) return PXA_STATUS_OK;
            if (status != PXA_STATUS_OK) { fault(s, now, status); continue; }
            /* Backend sequences must be monotonic for one subscription. */
            if (match.sequence && match.sequence <= s->last_sequence) continue;
            for (i = 0; i < s->subscribed_count; ++i)
                if (s->subscribed_keywords[i] == match.keyword_id) break;
            if (!match.sequence || !match.timestamp_us || match.timestamp_us > now ||
                match.revision != s->revision || i == s->subscribed_count ||
                match.timestamp_us < s->last_timestamp) {
                fault(s, now, PXA_STATUS_PROTOCOL_ERROR); continue;
            }
            if (now - match.timestamp_us > VT_MAX_EVENT_AGE_US) {
                fault(s, now, PXA_STATUS_TIMED_OUT); continue;
            }
            s->last_sequence = match.sequence;
            s->last_timestamp = match.timestamp_us;
            encode_event(s, PXA_VT_TRIGGERED, &match, PXA_STATUS_OK);
        }
    }
    return PXA_STATUS_OK;
}

void pxa_vt_service_deinit(pxa_vt_service_t *s) {
    if (!valid(s)) return;
    pxa_vt_service_set_foreground(s, PXA_COMPONENT_INVALID);
    /* Registration has runtime lifetime; the caller frees both together. */
    s->magic = 0;
}
