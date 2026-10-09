#include "lvgl.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static lv_indev_t *input;
static atomic_int pressed, callbacks, dispatching;
static unsigned char pixels[64*64*4];
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *map) {
    (void)area;(void)map;lv_display_flush_ready(display);
}
static void read_pointer(lv_indev_t *device,lv_indev_data_t *data) {
    (void)device;data->point.x=data->point.y=8;
    data->state=atomic_load(&pressed)?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void event(lv_event_t *e) {
    const lv_event_code_t code=lv_event_get_code(e);
    if(code!=LV_EVENT_CLICKED&&code!=LV_EVENT_VALUE_CHANGED)return;
    // Widen the overlap so an unprotected manual input read deterministically
    // collides with Host event dispatch, rather than relying on a rare crash.
    assert(atomic_fetch_add(&dispatching,1)==0);
    struct timespec delay={.tv_nsec=100000};nanosleep(&delay,NULL);
    atomic_fetch_add(&callbacks,1);assert(atomic_fetch_sub(&dispatching,1)==1);
}
static void *input_worker(void *context) {
    (void)context;
    for(int i=0;i<3000;++i){atomic_store(&pressed,i&1);lv_indev_read(input);}
    return NULL;
}
static void *host_worker(void *context) {
    lv_obj_t *screen=context;
    for(int i=0;i<1500;++i){
        lv_lock();lv_obj_t *obj=lv_obj_create(screen);
        lv_obj_add_event_cb(obj,event,LV_EVENT_VALUE_CHANGED,NULL);
        lv_obj_send_event(obj,LV_EVENT_VALUE_CHANGED,NULL);lv_obj_delete(obj);
        lv_unlock();
    }
    return NULL;
}
int main(void) {
    lv_init();lv_display_t *display=lv_display_create(64,64);assert(display);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *screen=lv_screen_active(),*button=lv_button_create(screen);
    lv_obj_set_pos(button,0,0);lv_obj_set_size(button,20,20);lv_obj_add_event_cb(button,event,LV_EVENT_CLICKED,NULL);
    input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);
    // Exercise both timer dispatch (recursive lock) and direct event wake.
    lv_tick_inc(20);lv_timer_handler();
    pthread_t reader,host;assert(!pthread_create(&reader,NULL,input_worker,NULL));assert(!pthread_create(&host,NULL,host_worker,screen));
    pthread_join(reader,NULL);pthread_join(host,NULL);
    assert(atomic_load(&callbacks)>=1500&&atomic_load(&dispatching)==0);
    lv_indev_delete(input);lv_display_delete(display);lv_deinit();
    printf("LVGL manual input and Host event/delete stress: %d serialized callbacks OK\n",atomic_load(&callbacks));
}
