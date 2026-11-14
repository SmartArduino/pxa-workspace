/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "pxa/pxa_host_interaction.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const pxa_interaction_control_t controls[] = {{"touchscreen", "触摸屏"}};
static const pxa_interaction_binding_t bindings[] = {{"tap", "touchscreen", PXA_INTERACTION_TAP, 0, -1}};
static const pxa_interaction_mode_t modes[] = {{"touch", "touch", bindings, 1}};
#define DESCRIPTOR(model_name, w, h) { \
    .struct_size=sizeof(pxa_host_interaction_t), .vendor="unknown", .model=model_name, \
    .hardware_revision="unknown", .firmware_id="test", .firmware_version="fixture", .build_id="fixture", \
    .profile_id="test.touch", .revision=1, .width=w, .height=h, .shape="rectangle", \
    .touch=1, .simultaneous=-1, .max_contacts=2, .controls=controls, .control_count=1, .modes=modes, .mode_count=1 }
static const pxa_host_interaction_t a = DESCRIPTOR("candidate-a", 296, 240);
static const pxa_host_interaction_t b = DESCRIPTOR("candidate-b", 480, 854);
typedef struct { uint32_t rotation, failure, calls; uint16_t width, height; } viewport_t;
static viewport_t view_a = {.width=296,.height=240}, view_b = {.width=480,.height=854};

static bool viewport(void *context, uint16_t *width, uint16_t *height) {
    viewport_t *view = context;
    assert(view == &view_a || view == &view_b);
    __atomic_add_fetch(&view->calls, 1, __ATOMIC_RELAXED);
    uint32_t failure = __atomic_load_n(&view->failure, __ATOMIC_RELAXED);
    if (failure == 1) { *width=99; *height=88; return false; }
    if (failure == 2) { *width=view->width; return true; }
    if (failure == 3) { *height=view->height; return true; }
    if (failure == 4) { *width=0; *height=view->height; return true; }
    if (failure == 5) { *width=view->width; *height=0; return true; }
    uint32_t rotation = __atomic_load_n(&view->rotation, __ATOMIC_RELAXED);
    *width = rotation & 1 ? view->height : view->width;
    *height = rotation & 1 ? view->width : view->height;
    return true;
}
static void expect_absent(void) {
    char output[32]; memset(output, 0xa5, sizeof(output)); size_t size=999;
    assert(!pxa_host_interaction_describe(output,sizeof(output),&size));
    assert(size==0 && output[0]==0 && (unsigned char)output[1]==0xa5);
}
static void print_snapshot(unsigned width, unsigned height) {
    char output[PXA_INTERACTION_MAX_BYTES+1], expected[80]; size_t size=999;
    uint32_t calls = view_a.calls;
    assert(pxa_host_interaction_describe(output,sizeof(output),&size));
    assert(size==strlen(output));
    snprintf(expected,sizeof(expected),"\"width\":%u,\"height\":%u",width,height);
    assert(strstr(output,expected)); assert(a.width==296 && a.height==240);
    if (view_a.calls) assert(view_a.calls==calls+1);
    puts(output);
}

static pthread_barrier_t barrier;
static uint32_t winners, winning_candidate, done, observations;
static void *register_candidate(void *candidate) {
    bool second = candidate != NULL;
    pthread_barrier_wait(&barrier);
    bool ok = pxa_host_interaction_register_with_viewport(second ? &b : &a,
                                                        viewport, second ? &view_b : &view_a);
    if (ok) {
        __atomic_store_n(&winning_candidate,second ? 2u : 1u,__ATOMIC_RELEASE);
        __atomic_add_fetch(&winners,1,__ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void *observe(void *unused) {
    (void)unused;
    unsigned first_candidate=0;
    for (;;) {
        bool finished = __atomic_load_n(&done,__ATOMIC_ACQUIRE) == 2;
        char output[PXA_INTERACTION_MAX_BYTES+1]; size_t size=999;
        memset(output,0xa5,sizeof(output));
        if (pxa_host_interaction_describe(output,sizeof(output),&size)) {
            bool is_a = strstr(output,"\"model\":\"candidate-a\"") != NULL;
            bool is_b = strstr(output,"\"model\":\"candidate-b\"") != NULL;
            assert(is_a != is_b);
            unsigned candidate=is_a ? 1u:2u;
            if (!first_candidate) first_candidate=candidate;
            assert(candidate==first_candidate);
            assert(strstr(output,is_a ? "\"width\":296,\"height\":240" : "\"width\":480,\"height\":854"));
            assert(size==strlen(output));
            __atomic_add_fetch(&observations,1,__ATOMIC_RELAXED);
        } else { assert(!finished); assert(size==0 && output[0]==0); }
        if (finished) break;
    }
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc==2); expect_absent();
    if (!strcmp(argv[1],"race")) {
        pthread_t writers[2], readers[2];
        assert(!pthread_barrier_init(&barrier,NULL,3));
        assert(!pthread_create(&readers[0],NULL,observe,NULL));
        assert(!pthread_create(&readers[1],NULL,observe,NULL));
        assert(!pthread_create(&writers[0],NULL,register_candidate,NULL));
        assert(!pthread_create(&writers[1],NULL,register_candidate,(void*)1));
        pthread_barrier_wait(&barrier);
        for (unsigned i=0;i<2;++i) { assert(!pthread_join(writers[i],NULL)); assert(!pthread_join(readers[i],NULL)); }
        assert(winners==1 && observations>0);
        char output[PXA_INTERACTION_MAX_BYTES+1];size_t size=999;
        assert(pxa_host_interaction_describe(output,sizeof(output),&size));
        bool won_a=__atomic_load_n(&winning_candidate,__ATOMIC_ACQUIRE)==1;
        assert(strstr(output,won_a ? "\"model\":\"candidate-a\"" : "\"model\":\"candidate-b\""));
        assert(strstr(output,won_a ? "\"width\":296,\"height\":240" : "\"width\":480,\"height\":854"));
        assert(!pxa_host_interaction_register(&a));
        assert(!pxa_host_interaction_register_with_viewport(&b,viewport,&view_b));
        puts("race: exactly one coherent descriptor/callback/context published");
        return 0;
    }
    if (!strcmp(argv[1],"static") || !strcmp(argv[1],"null-query")) {
        assert(!pxa_host_interaction_register(NULL));
        assert(!pxa_host_interaction_register_with_viewport(NULL,viewport,&view_a));
        pxa_host_interaction_t invalid=a; invalid.struct_size=0;
        assert(!pxa_host_interaction_register_with_viewport(&invalid,viewport,&view_a));
        invalid=a; invalid.width=0; assert(!pxa_host_interaction_register(&invalid));
        if (!strcmp(argv[1],"static")) assert(pxa_host_interaction_register(&a));
        else assert(pxa_host_interaction_register_with_viewport(&a,NULL,&view_b));
        assert(!pxa_host_interaction_register_with_viewport(&b,viewport,&view_b));
        view_a.rotation=1; print_snapshot(296,240); assert(!view_a.calls && !view_b.calls);
        return 0;
    }
    assert(!strcmp(argv[1],"live"));
    assert(pxa_host_interaction_register_with_viewport(&a,viewport,&view_a));
    assert(!pxa_host_interaction_register(&b));
    assert(!pxa_host_interaction_register_with_viewport(&b,viewport,&view_b));
    for (unsigned rotation=0;rotation<4;++rotation) {
        view_a.rotation=rotation; print_snapshot(rotation&1 ? 240:296,rotation&1 ? 296:240);
    }
    for (unsigned failure=1;failure<=5;++failure) { view_a.failure=failure; expect_absent(); }
    view_a.failure=0; view_a.rotation=0; print_snapshot(296,240);
    for (size_t capacity=1;capacity<1000;++capacity) {
        char output[PXA_INTERACTION_MAX_BYTES+1]; memset(output,0xa5,sizeof(output)); size_t size=999;
        uint32_t calls=view_a.calls;
        bool ok=pxa_host_interaction_describe(output,capacity,&size);
        assert(view_a.calls==calls+1 && (unsigned char)output[capacity]==0xa5);
        assert(ok ? size<capacity && strlen(output)==size : size==0 && output[0]==0);
    }
    size_t size=999; uint32_t calls=view_a.calls;
    assert(!pxa_host_interaction_describe(NULL,100,&size) && size==0);
    char output[2]={'x','y'};
    assert(!pxa_host_interaction_describe(output,0,&size) && size==0 && output[0]=='x');
    assert(!pxa_host_interaction_describe(output,sizeof(output),NULL) && output[0]==0 && output[1]=='y');
    assert(view_a.calls==calls && !view_b.calls);
    return 0;
}
