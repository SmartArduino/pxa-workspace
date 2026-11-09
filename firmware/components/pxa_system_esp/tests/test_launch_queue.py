#!/usr/bin/env python3
"""The LVGL owner resolves newly installed apps before starting queued requests."""
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
#define LV_RESULT_OK 0
typedef int pxsys_status_t;
typedef int pxsys_instance_ref_t;
typedef struct {const char* data;size_t size;} pxsys_string_t;
typedef struct {uint8_t publisher_root[32];pxsys_string_t app_id;}pxsys_app_identity_t;
typedef struct {unsigned struct_size;pxsys_string_t action;const pxsys_app_identity_t*target;}pxsys_intent_t;
typedef struct {pxsys_app_identity_t identity;char app_id[65];}tracked_t;
typedef struct esp_pxa_instance {struct esp_pxa_instance*next;uint8_t publisher_root[32];char app_id[65],identity_key[140];uint64_t host_instance_id;}esp_pxa_instance_t;
typedef struct {int valid;uint32_t generation;void*system;size_t tracked_count;tracked_t tracked[1];esp_pxa_instance_t*instances;}pxsys_esp_pxa_bridge_t;
static pxsys_esp_pxa_bridge_t*g_bridge;
typedef int pxsys_app_registry_t;
static int owner,registered,can_register=1,queue_fails,starts,syncs;
static int host_active,timer_fails;static uint32_t tick;
static void(*pending)(void*);static void*pending_context;
static uint8_t last_root[32];static char last_id[65];
static pxsys_string_t pxsys_string_from_cstr(const char*s){return(pxsys_string_t){s,strlen(s)};}
static int bridge_valid(const pxsys_esp_pxa_bridge_t*b){return b&&b->valid;}
static pxsys_app_registry_t*pxsys_standard_system_apps(void*s){assert(owner);return s;}
static void*pxsys_standard_system_tasks(void*s){assert(owner);return s;}
static void*pxsys_app_registry_find(pxsys_app_registry_t*a,const pxsys_app_identity_t*t){(void)t;assert(owner);return registered?a:NULL;}
static pxsys_status_t pxsys_esp_pxa_bridge_sync(pxsys_esp_pxa_bridge_t*b){assert(owner&&b);++syncs;registered=can_register;return registered?0:-1;}
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
static uint32_t lv_tick_get(void){return tick;}
static uint32_t lv_tick_elaps(uint32_t start){return tick-start;}
static void*lv_timer_get_user_data(lv_timer_t*t){return t->context;}
static void fire_timer(void*context){lv_timer_t*t=context;tick+=20;t->callback(t);}
static lv_timer_t*lv_timer_create(void(*callback)(lv_timer_t*),uint32_t ms,void*context){
 assert(owner&&!pending&&ms==20);if(timer_fails)return NULL;
 timer.context=context;timer.callback=callback;pending=fire_timer;pending_context=&timer;return &timer;
}
static void lv_timer_set_repeat_count(lv_timer_t*t,int count){assert(t==&timer&&count==1);}
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
 assert(launch_requested(&bridge,root,id));drain();assert(starts==5&&!pending);
 host_active=0;tick=UINT32_MAX-10;
 assert(launch_requested(&bridge,root,id));drain();drain();assert(starts==5&&pending);
 tick+=3000;drain();assert(starts==5&&!pending);
 timer_fails=1;assert(launch_requested(&bridge,root,id));drain();assert(starts==5&&!pending);timer_fails=0;
 assert(launch_requested(&bridge,root,id));drain();++bridge.generation;drain();assert(starts==5&&!pending);
 old.publisher_root[0]^=1;
 assert(launch_requested(&bridge,root,id));drain();assert(starts==6&&!pending);
 pxsys_esp_pxa_bridge_t*retired=malloc(sizeof(*retired));assert(retired);*retired=bridge;g_bridge=retired;
 assert(launch_requested(retired,root,id));g_bridge=NULL;free(retired);drain();assert(starts==6&&!pending);
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
 with tempfile.TemporaryDirectory(prefix='pxa-launch-')as d:
  p=Path(d);(p/'test.c').write_text(FIXTURE.replace('// PRODUCTION',body))
  subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':main()
