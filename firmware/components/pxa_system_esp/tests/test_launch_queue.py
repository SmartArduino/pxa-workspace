#!/usr/bin/env python3
"""Exercise queued launch and deferred cancellation on the LVGL owner."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'firmware/components/pxa_integration/tests'))
from test_startup import function
FIXTURE=r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PXA_HOST_PUBLISHER_ROOT_BYTES 32
#define PXSYS_PUBLISHER_ROOT_BYTES 32
#define PXA_HOST_APP_ID_MAX 65
#define PXSYS_ESP_PXA_TAG "test"
#define ESP_LOGW(...) ((void)0)
#define PXSYS_STATUS_OK 0
#define PXSYS_STATUS_PENDING 1
#define PXSYS_STATUS_INTERNAL -1
#define PXSYS_STATUS_NOT_FOUND -2
#define PXSYS_STATUS_TIMEOUT -3
#define PXSYS_STATUS_NO_MEMORY -4
#define PXSYS_STATUS_INVALID_ARGUMENT -5
#define PXSYS_STATUS_UNAVAILABLE -6
#define LV_RESULT_OK 0
typedef int pxsys_status_t;
typedef int pxsys_instance_ref_t;
typedef uint8_t pxsys_stop_reason_t;
typedef struct {const char* data;size_t size;} pxsys_string_t;
typedef struct {uint8_t publisher_root[32];pxsys_string_t app_id;}pxsys_app_identity_t;
typedef struct {unsigned struct_size;pxsys_string_t action;const pxsys_app_identity_t*target;}pxsys_intent_t;
typedef struct {pxsys_app_identity_t identity;char app_id[65];}tracked_t;
typedef struct esp_pxa_instance {
 struct esp_pxa_instance*next;uint8_t publisher_root[32];char app_id[65],identity_key[140];
 uint64_t host_instance_id;uint8_t*launch_event;struct lv_timer*start_stop_timer;
 uint8_t start_cancelled;pxsys_stop_reason_t start_stop_reason;pxsys_instance_ref_t reference;
}esp_pxa_instance_t;
typedef struct {int valid;uint32_t generation;void*system;size_t tracked_count;tracked_t tracked[1];esp_pxa_instance_t*instances,*window_instance;}pxsys_esp_pxa_bridge_t;
static pxsys_esp_pxa_bridge_t*g_bridge;
typedef int pxsys_app_registry_t;
typedef int pxsys_task_manager_t;
static int owner,registered,can_register=1,queue_fails,starts,resumes,syncs;
static int host_active,timer_fails;static uint32_t tick;
static int host_stop_accepts,host_stop_requests,stopped_reports,surface_hides;
static void(*pending)(void*);static void*pending_context;
static uint8_t last_root[32];static char last_id[65];
static pxsys_string_t pxsys_string_from_cstr(const char*s){return(pxsys_string_t){s,strlen(s)};}
static int bridge_valid(const pxsys_esp_pxa_bridge_t*b){return b&&b->valid;}
static pxsys_app_registry_t*pxsys_standard_system_apps(void*s){assert(owner);return s;}
static pxsys_task_manager_t*pxsys_standard_system_tasks(void*s){assert(owner);return s;}
static void*pxsys_standard_system_role_host(void*s){assert(owner);return s;}
static pxsys_status_t pxsys_task_manager_report_stopped(void*s,pxsys_instance_ref_t ref,pxsys_stop_reason_t reason){
 assert(owner&&s&&ref==5&&reason==4);++stopped_reports;return PXSYS_STATUS_OK;
}
static pxsys_status_t pxsys_role_host_report_stopped(void*s,pxsys_instance_ref_t ref,pxsys_stop_reason_t reason){
 (void)s;(void)ref;(void)reason;assert(!"unexpected role callback");return PXSYS_STATUS_NOT_FOUND;
}
static void*pxsys_app_registry_find(pxsys_app_registry_t*a,const pxsys_app_identity_t*t){(void)t;assert(owner);return registered?a:NULL;}
static pxsys_status_t pxsys_esp_pxa_bridge_sync(pxsys_esp_pxa_bridge_t*b){assert(owner&&b);++syncs;registered=can_register;return registered?0:-1;}
static pxsys_status_t pxsys_task_manager_resume_app(pxsys_task_manager_t*s,const pxsys_app_identity_t*id){
 assert(owner&&s&&id);
 if(!host_active)return PXSYS_STATUS_NOT_FOUND;
 ++resumes;return PXSYS_STATUS_OK;
}
static pxsys_status_t pxsys_task_manager_start(void*s,const pxsys_intent_t*i,pxsys_instance_ref_t*out){
 assert(owner&&s&&i->struct_size==sizeof(*i)&&out);++starts;
 assert(!strcmp(i->action.data,"system.intent.main"));
 memcpy(last_root,i->target->publisher_root,32);assert(i->target->app_id.size<sizeof(last_id));
 memcpy(last_id,i->target->app_id.data,i->target->app_id.size);last_id[i->target->app_id.size]=0;
 return registered?0:PXSYS_STATUS_NOT_FOUND;
}
static int schedule_on_lvgl_owner(void(*callback)(void*),void*context){
 assert(!pending);if(queue_fails)return -1;pending=callback;pending_context=context;return 0;
}
typedef struct lv_timer {void*context;void(*callback)(struct lv_timer*);}lv_timer_t;
static lv_timer_t timer;
static int pxa_host_runtime_is_instance_active(const char*id,uint64_t generation){assert(owner&&id&&generation==8);return host_active;}
static int pxa_host_runtime_stop_with_reason(const char*id,uint64_t generation,uint8_t reason){
 assert(owner&&strcmp(id,"full:identity")==0&&generation==8&&reason==4);++host_stop_requests;
 return host_stop_accepts;
}
static void reset_window(pxsys_esp_pxa_bridge_t*b){assert(owner);b->window_instance=NULL;}
static void pxa_esp_surface_set_host_visible(bool visible){assert(owner&&!visible);++surface_hides;}
static uint32_t lv_tick_get(void){return tick;}
static uint32_t lv_tick_elaps(uint32_t start){return tick-start;}
static void*lv_timer_get_user_data(lv_timer_t*t){return t->context;}
static void fire_timer(void*context){lv_timer_t*t=context;tick+=20;t->callback(t);}
static lv_timer_t*lv_timer_create(void(*callback)(lv_timer_t*),uint32_t ms,void*context){
 assert(owner&&!pending&&ms==20);if(timer_fails)return NULL;
 timer.context=context;timer.callback=callback;pending=fire_timer;pending_context=&timer;return &timer;
}
static void lv_timer_set_repeat_count(lv_timer_t*t,int count){assert(t==&timer&&count==1);}
static void lv_timer_pause(lv_timer_t*t){assert(t==&timer);pending=NULL;}
static void lv_timer_resume(lv_timer_t*t){assert(t==&timer&&!pending);pending=fire_timer;pending_context=t;}
static void lv_timer_delete(lv_timer_t*t){lv_timer_pause(t);t->context=NULL;}
// PRODUCTION
static void drain(void){assert(pending);owner=1;void(*cb)(void*)=pending;void*ctx=pending_context;pending=NULL;cb(ctx);owner=0;}
int main(void){
 int system=0;pxsys_esp_pxa_bridge_t bridge={.valid=1,.generation=7,.system=&system};
 g_bridge=&bridge;
 uint8_t root[32];memset(root,42,32);char id[65]="pxa-just-installed";
 /* Empty tracked snapshot reproduces the install->immediate launch race. */
 assert(launch_requested(&bridge,root,id));assert(!syncs&&!starts);
 memset(root,0,32);memset(id,0,sizeof(id));drain();
 assert(syncs==1&&starts==1&&last_root[0]==42&&!strcmp(last_id,"pxa-just-installed"));
 memset(root,42,32);strcpy(id,"pxa-existing");
 assert(launch_requested(&bridge,root,id));drain();assert(syncs==1&&starts==2);
 assert(launch_requested(&bridge,root,id));++bridge.generation;drain();assert(starts==2);
 queue_fails=1;assert(!launch_requested(&bridge,root,id));assert(!pending);queue_fails=0;
 assert(!launch_requested(NULL,root,id));assert(!launch_requested(&bridge,NULL,id));
 assert(!launch_requested(&bridge,root,NULL));assert(!launch_requested(&bridge,root,""));
 memset(id,'a',sizeof(id));assert(!launch_requested(&bridge,root,id));
 registered=0;can_register=0;strcpy(id,"pxa-gone-before-start");
 assert(launch_requested(&bridge,root,id));drain();assert(syncs==2&&starts==3&&!registered);
 registered=1;esp_pxa_instance_t old={.host_instance_id=8};
 memcpy(old.publisher_root,root,32);strcpy(old.app_id,id);strcpy(old.identity_key,"full:identity");bridge.instances=&old;
 assert(launch_requested(&bridge,root,id));drain();assert(starts==3&&pending);
 drain();assert(starts==3&&pending); /* Still waiting: never deliver to a retired Host instance. */
 bridge.instances=NULL;drain();assert(starts==4&&!pending); /* STOPPED processed, one new start. */
 bridge.instances=&old;host_active=1;
 assert(launch_requested(&bridge,root,id));drain();assert(starts==4&&resumes==1&&!pending);
 host_active=0;tick=UINT32_MAX-10;
 assert(launch_requested(&bridge,root,id));drain();drain();assert(starts==4&&pending);
 tick+=3000;drain();assert(starts==4&&!pending);
 timer_fails=1;assert(launch_requested(&bridge,root,id));drain();assert(starts==4&&!pending);timer_fails=0;
 assert(launch_requested(&bridge,root,id));drain();++bridge.generation;drain();assert(starts==4&&!pending);
 old.publisher_root[0]^=1;
 assert(launch_requested(&bridge,root,id));drain();assert(starts==5&&!pending);
 pxsys_esp_pxa_bridge_t*retired=malloc(sizeof(*retired));assert(retired);*retired=bridge;g_bridge=retired;
 assert(launch_requested(retired,root,id));g_bridge=NULL;free(retired);drain();assert(starts==5&&!pending);
 g_bridge=&bridge;
 owner=1;
 uint8_t intent_byte=1;
 old.reference=5;old.host_instance_id=0;old.launch_event=&intent_byte;bridge.window_instance=&old;
 timer_fails=1;
 assert(backend_request_stop(&bridge,&old,4)==PXSYS_STATUS_NO_MEMORY);
 assert(bridge.window_instance==&old&&!old.start_cancelled);
 timer_fails=0;
 assert(backend_request_stop(&bridge,&old,4)==PXSYS_STATUS_PENDING);
 assert(old.start_cancelled&&old.start_stop_timer==&timer&&!pending&&!host_stop_requests);
 assert(!bridge.window_instance);
 old.host_instance_id=8;host_active=1;
 stop_cancelled_start(old.start_stop_timer);
 assert(pending&&host_stop_requests==1);
 host_stop_accepts=1;owner=0;drain();
 assert(!old.start_stop_timer&&host_stop_requests==2&&!pending);
 owner=1;
 old.launch_event=NULL;old.start_cancelled=0;bridge.window_instance=&old;host_stop_accepts=0;
 assert(backend_request_stop(&bridge,&old,4)==PXSYS_STATUS_UNAVAILABLE);
 assert(bridge.window_instance==&old&&!surface_hides);
 host_stop_accepts=1;
 assert(backend_request_stop(&bridge,&old,4)==PXSYS_STATUS_PENDING);
 assert(!bridge.window_instance&&surface_hides==1);
 old.host_instance_id=0;old.launch_event=&intent_byte;
 assert(backend_request_stop(&bridge,&old,4)==PXSYS_STATUS_PENDING);
 old.host_instance_id=8;host_active=0;
 stop_cancelled_start(old.start_stop_timer);
 assert(stopped_reports==1&&host_stop_requests==4);
 puts("Queued launch: fresh catalog, owner-only lookup, stop completion, timeout, cancellation and allocation failure passed");
}
"""
def main():
 source=(ROOT/'firmware/components/pxa_system_esp/src/esp_pxa_bridge.c').read_text()
 begin=source.index('typedef struct {',source.index('/* A launch request may arrive'))
 end=source.index('} pending_launch_t;',begin)+len('} pending_launch_t;')
 timer='static void continue_launch_request(lv_timer_t* timer) {'
 body=source[begin:end]+'\nstatic void perform_launch_request(void* context);\n'
 if timer in source:body+=function(source,timer)+'\n'
 body+=function(source,'static void perform_launch_request(')+'\n'+function(source,'static bool launch_requested(')
 for signature in ('static void report_instance_stopped(', 'static void stop_cancelled_start(',
                   'static pxsys_status_t backend_request_stop('):
  body+='\n'+function(source,signature)
 with tempfile.TemporaryDirectory(prefix='pxa-launch-')as d:
  p=Path(d);(p/'test.c').write_text(FIXTURE.replace('// PRODUCTION',body))
  subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':main()
