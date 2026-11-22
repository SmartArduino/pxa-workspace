// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_threads.h"
#include "pxa_video_memory.h"
#include "esp_pthread.h"
#include "esp_heap_caps.h"
#include <array>
#include <cerrno>

namespace {
// Fixed native Host infrastructure, like the Net worker: at most two 32 KiB
// PSRAM stacks. Codec data/control allocations still use the captured app
// allocator. Join retires every worker before that allocator can be recycled.
struct Worker {
    pthread_t thread{};
    PxaVideoThreadAllocators allocators{};
    void *(*routine)(void *)=nullptr;
    void *argument=nullptr;
};
std::array<Worker *,2> workers{};
void *run(void *value) {
    auto *w=static_cast<Worker *>(value);
    pxa_video_thread_allocators(w->allocators);
    void *result=w->routine(w->argument);
    pxa_video_thread_allocators({nullptr,nullptr});
    return result;
}
}
int pxa_video_pthread_create(pthread_t *thread,const pthread_attr_t *,void *(*routine)(void *),void *argument) {
    unsigned slot=0;while(slot<workers.size() && workers[slot])++slot;
    if(slot==workers.size() || heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<16*1024)
        return EAGAIN;
    auto *w=pxa_video_new<Worker>();if(!w)return ENOMEM;
    w->allocators=pxa_video_thread_allocators();w->routine=routine;w->argument=argument;
    esp_pthread_cfg_t previous{};
    if(esp_pthread_get_cfg(&previous)!=ESP_OK)previous=esp_pthread_get_default_config();
    auto cfg=esp_pthread_get_default_config();cfg.stack_size=32*1024;
    cfg.stack_alloc_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    cfg.pin_to_core=slot;cfg.prio=5;cfg.thread_name="PxaH264";cfg.inherit_cfg=false;
    int result=ENOMEM;
    if(esp_pthread_set_cfg(&cfg)==ESP_OK) {
        pthread_attr_t attributes;
        result=pthread_attr_init(&attributes);
        if(!result) {
            result=pthread_attr_setstacksize(&attributes,cfg.stack_size);
            if(!result)result=pthread_create(thread,&attributes,run,w);
            pthread_attr_destroy(&attributes);
        }
    }
    (void)esp_pthread_set_cfg(&previous);
    if(result)pxa_video_delete(w);
    else {w->thread=*thread;workers[slot]=w;}
    return result;
}
int pxa_video_pthread_join(pthread_t thread,void **result) {
    int status=pthread_join(thread,result);
    if(!status)for(auto& w:workers)if(w && pthread_equal(w->thread,thread)) {
        pxa_video_delete(w);w=nullptr;break;
    }
    return status;
}
