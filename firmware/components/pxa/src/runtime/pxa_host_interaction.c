/* SPDX-License-Identifier: MIT */
#include "pxa/pxa_host_interaction.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const pxa_host_interaction_t *registered;
typedef struct { char *bytes; size_t capacity, size; bool ok; } writer_t;
static void put(writer_t *w, const char *format, ...) {
    if (!w->ok) return;
    va_list args; va_start(args, format);
    int count = vsnprintf(w->bytes + w->size, w->capacity - w->size, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= w->capacity - w->size) { w->ok = false; return; }
    w->size += (size_t)count;
}
static void string(writer_t *w, const char *text) {
    if (!text) { w->ok = false; return; }
    put(w, "\"");
    size_t i;
    for (i=0; i<129 && text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32) { w->ok = false; return; }
        if (c == '"' || c == '\\') put(w, "\\%c", c);
        else put(w, "%c", c);
    }
    if (!i || i > 128) w->ok = false;
    put(w, "\"");
}
static void field(writer_t *w, const char *key, const char *value) {
    put(w, "\"%s\":", key); string(w, value);
}
static void tri(writer_t *w, int value) {
    if (value < -1 || value > 1) w->ok = false;
    put(w, value < 0 ? "null" : value ? "true" : "false");
}
static bool valid_shape(const char *shape) {
    return shape && (!strcmp(shape,"rectangle") || !strcmp(shape,"rounded") ||
                     !strcmp(shape,"circle") || !strcmp(shape,"unknown"));
}
static bool valid_kind(const char *kind) {
    return kind && (!strcmp(kind,"focus") || !strcmp(kind,"controller") ||
                    !strcmp(kind,"touch") || !strcmp(kind,"pointer"));
}
bool pxa_host_interaction_register(const pxa_host_interaction_t *d) {
    if (!d || d->struct_size != sizeof(*d) || registered || !d->revision ||
        !d->width || !d->height || !valid_shape(d->shape) ||
        d->control_count > 32 || d->mode_count > 16 || d->exclusive_count > 16 || d->reserved_count > 32 ||
        (d->control_count && !d->controls) || (d->mode_count && !d->modes) ||
        (d->exclusive_count && !d->exclusive) || (d->reserved_count && !d->reserved)) return false;
    for (size_t i=0; i<d->mode_count; ++i)
        if (!valid_kind(d->modes[i].kind) || !d->modes[i].bindings ||
            !d->modes[i].binding_count || d->modes[i].binding_count > 32) return false;
    for (size_t i=0; i<d->exclusive_count; ++i)
        if (!d->exclusive[i].controls || d->exclusive[i].count < 2 || d->exclusive[i].count > 8) return false;
    registered = d;
    return true;
}
bool pxa_host_interaction_describe(char *output, size_t capacity, size_t *output_size) {
    if (output_size) *output_size = 0;
    if (!output || !capacity || !output_size || !registered) return false;
    const pxa_host_interaction_t *d = registered;
    writer_t w = {output, capacity < PXA_INTERACTION_MAX_BYTES+1 ? capacity : PXA_INTERACTION_MAX_BYTES+1, 0, true};
    put(&w,"{\"schema\":\"pxa-device-interaction-1\",\"identity\":{");
    field(&w,"vendor",d->vendor); put(&w,","); field(&w,"model",d->model); put(&w,",");
    field(&w,"hardware_revision",d->hardware_revision); put(&w,","); field(&w,"firmware_id",d->firmware_id); put(&w,",");
    field(&w,"firmware_version",d->firmware_version); put(&w,","); field(&w,"build_id",d->build_id);
    put(&w,"},\"profile\":{"); field(&w,"id",d->profile_id); put(&w,",\"revision\":%u},",(unsigned)d->revision);
    put(&w,"\"display\":{\"width\":%u,\"height\":%u,",d->width,d->height); field(&w,"shape",d->shape);
    put(&w,"},\"physical\":{\"touch\":"); tri(&w,d->touch);
    put(&w,",\"max_contacts\":");
    if (d->max_contacts < 0) put(&w,"null"); else put(&w,"%d",d->max_contacts);
    put(&w,",\"simultaneous\":"); tri(&w,d->simultaneous); put(&w,",\"controls\":[");
    for (size_t i=0; i<d->control_count; ++i) {
        if(i)put(&w,",");
        put(&w,"{"); field(&w,"id",d->controls[i].id); put(&w,",");
        field(&w,"label",d->controls[i].label); put(&w,"}");
    }
    put(&w,"],\"exclusive_groups\":[");
    for(size_t i=0;i<d->exclusive_count;++i) {
        if(i)put(&w,",");
        put(&w,"[");
        for(size_t j=0;j<d->exclusive[i].count;++j){if(j)put(&w,",");string(&w,d->exclusive[i].controls[j]);}
        put(&w,"]");
    }
    put(&w,"]},\"modes\":[");
    static const char *events[] = {"tap","press","release","hold","repeat","double_tap"};
    for(size_t i=0;i<d->mode_count;++i) {
        const pxa_interaction_mode_t *mode=&d->modes[i];
        if(i)put(&w,",");
        put(&w,"{"); field(&w,"id",mode->id); put(&w,","); field(&w,"kind",mode->kind);
        put(&w,",\"actions\":{");
        for(size_t j=0;j<mode->binding_count;++j) {
            const pxa_interaction_binding_t *binding=&mode->bindings[j];
            if(j)put(&w,",");
        string(&w,binding->action); put(&w,":{"); field(&w,"control",binding->control);
            put(&w,",\"events\":["); unsigned count=0;
            if (!binding->events || (binding->events & ~63u)) w.ok=false;
            for(unsigned k=0;k<6;++k)if(binding->events&(1u<<k)){if(count++)put(&w,",");string(&w,events[k]);}
            put(&w,"],\"hold_limit_ms\":");
            if(binding->hold_limit_ms < -1)put(&w,"null"); else put(&w,"%ld",(long)binding->hold_limit_ms);
            put(&w,",\"tap_latency_ms\":");
            if(binding->tap_latency_ms < 0)put(&w,"null"); else put(&w,"%ld",(long)binding->tap_latency_ms);
            put(&w,"}");
        }
        put(&w,"}}");
    }
    put(&w,"],\"reserved\":[");
    for(size_t i=0;i<d->reserved_count;++i) {
        if(i)put(&w,",");
        put(&w,"{"); field(&w,"control",d->reserved[i].control); put(&w,",");
        field(&w,"event",d->reserved[i].event); put(&w,","); field(&w,"action",d->reserved[i].action);
        put(&w,",\"threshold_ms\":"); if(d->reserved[i].threshold_ms)put(&w,"%u",(unsigned)d->reserved[i].threshold_ms);else put(&w,"null");
        put(&w,"}");
    }
    put(&w,"],\"runtime\":{"); field(&w,"context",d->runtime_context?d->runtime_context(d->context):"unknown"); put(&w,"}}");
    if (!w.ok) { output[0]=0; return false; }
    *output_size=w.size;
    return true;
}
