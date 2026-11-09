#!/usr/bin/env python3
"""Run the production station against a deterministic ESP event queue."""
import os
from pathlib import Path
import subprocess
import tempfile

STUB = r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_WIFI_STATE=2;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(value) assert((value)==ESP_OK)
#define BIT0 1
#define BIT1 2
#define BIT2 4
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1
using EventBits_t=uint32_t;
using EventGroupHandle_t=uint32_t*;
inline auto xEventGroupCreate(){return new uint32_t(0);}
inline void vEventGroupDelete(EventGroupHandle_t g){delete g;}
inline auto xEventGroupGetBits(EventGroupHandle_t g){return *g;}
inline auto xEventGroupClearBits(EventGroupHandle_t g,uint32_t b){*g&=~b;return *g;}
inline auto xEventGroupSetBits(EventGroupHandle_t g,uint32_t b){*g|=b;return *g;}
inline auto xEventGroupWaitBits(EventGroupHandle_t g,uint32_t,int,int,int){return *g;}
using esp_event_base_t=const char*;
using esp_event_handler_instance_t=void*;
using Handler=void(*)(void*,esp_event_base_t,int32_t,void*);
inline const char* WIFI_EVENT="wifi";
inline const char* IP_EVENT="ip";
constexpr int ESP_EVENT_ANY_ID=-1, WIFI_EVENT_STA_START=1, WIFI_EVENT_SCAN_DONE=2;
constexpr int WIFI_EVENT_STA_DISCONNECTED=3, WIFI_EVENT_STA_CONNECTED=4, IP_EVENT_STA_GOT_IP=5;
constexpr int WIFI_REASON_ASSOC_LEAVE=8;
#define ESP_EVENT_DEFINE_BASE(name) const char* name=#name
struct Registration{esp_event_base_t base;int id;Handler fn;void* owner;};
inline std::vector<Registration*> registrations;
struct Event{esp_event_base_t base;int id;std::vector<unsigned char> data;};
inline std::vector<Event> events;
inline bool fail_post=false;
inline int esp_event_handler_instance_register(esp_event_base_t b,int id,Handler fn,void* owner,void** result){
 auto* r=new Registration{b,id,fn,owner};registrations.push_back(r);*result=r;return ESP_OK;
}
inline int esp_event_handler_instance_unregister(esp_event_base_t,int,void* token){
 auto* r=static_cast<Registration*>(token);registrations.erase(std::find(registrations.begin(),registrations.end(),r));delete r;return ESP_OK;
}
inline int esp_event_post(esp_event_base_t b,int id,const void* data,size_t n,int){
 if(fail_post)return ESP_FAIL;
 const auto* bytes=static_cast<const unsigned char*>(data);
 events.push_back({b,id,{bytes,bytes+n}});return ESP_OK;
}
inline void deliver(esp_event_base_t b,int id,void* data){
 for(auto* r:registrations)if(r->base==b && (r->id==id || r->id==ESP_EVENT_ANY_ID))r->fn(r->owner,b,id,data);
}
inline void pump(){
 while(!events.empty()){auto e=events.front();events.erase(events.begin());deliver(e.base,e.id,e.data.data());}
}
struct esp_timer_create_args_t {void(*callback)(void*);void* arg;int dispatch_method;const char* name;bool skip_unhandled_events;};
using esp_timer_handle_t=int*;
constexpr int ESP_TIMER_TASK=0;
inline int64_t esp_timer_get_time(){return 1000000;}
inline int esp_timer_create(const esp_timer_create_args_t*,esp_timer_handle_t* h){*h=new int(0);return ESP_OK;}
inline int esp_timer_stop(esp_timer_handle_t){return ESP_OK;}
inline int esp_timer_delete(esp_timer_handle_t h){delete h;return ESP_OK;}
inline int esp_timer_start_once(esp_timer_handle_t,int64_t){return ESP_OK;}
using nvs_handle_t=int;
constexpr int NVS_READONLY=0;
inline int nvs_open(const char*,int,int*){return ESP_FAIL;}
inline int nvs_get_i8(int,const char*,int8_t*){return ESP_FAIL;}
inline int nvs_get_u8(int,const char*,uint8_t*){return ESP_FAIL;}
inline void nvs_close(int){}
inline const char* esp_err_to_name(int){return "fake-error";}
using wifi_auth_mode_t=int;
constexpr int WIFI_IF_STA=0, WIFI_MODE_STA=0, WIFI_AUTH_OPEN=0;
struct wifi_ap_record_t {uint8_t ssid[33]{};int8_t rssi=-40;int authmode=1;uint8_t bssid[6]{};uint8_t primary=1;};
struct wifi_config_t {struct {uint8_t ssid[32],password[64];int listen_interval,channel;uint8_t bssid[6];bool bssid_set;}sta;};
struct wifi_event_sta_disconnected_t {int reason;};
using wifi_ps_type_t=int;
constexpr int WIFI_PS_MAX_MODEM=0,WIFI_PS_MIN_MODEM=1,WIFI_PS_NONE=2;
inline int radio_connects=0,radio_disconnects=0,radio_scans=0,radio_configs=0,saves=0;
inline int configure_result=ESP_OK;
inline wifi_config_t last_config{};
inline int esp_wifi_set_config(int,const wifi_config_t* c){++radio_configs;last_config=*c;return configure_result;}
inline int esp_wifi_disconnect(){++radio_disconnects;wifi_event_sta_disconnected_t e{8};return esp_event_post(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e,sizeof(e),0);}
inline int esp_wifi_connect(){++radio_connects;return ESP_OK;}
inline int esp_wifi_scan_start(void*,bool){++radio_scans;return ESP_OK;}
inline int esp_wifi_scan_stop(){return ESP_OK;}
inline int esp_wifi_stop(){return ESP_OK;}
inline int esp_wifi_start(){return ESP_OK;}
inline int esp_wifi_set_mode(int){return ESP_OK;}
inline int esp_wifi_set_max_tx_power(int){return ESP_OK;}
inline int esp_wifi_set_ps(int){return ESP_OK;}
inline int esp_wifi_scan_get_ap_num(uint16_t* n){*n=1;return ESP_OK;}
inline int esp_wifi_scan_get_ap_records(uint16_t*,wifi_ap_record_t* a){*a={};std::strcpy(reinterpret_cast<char*>(a->ssid),"example-net");return ESP_OK;}
inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t* a){*a={};return ESP_OK;}
using esp_netif_t=int;
inline auto esp_netif_create_default_wifi_sta(){return new int(0);}
inline void esp_netif_destroy_default_wifi(esp_netif_t* n){delete n;}
struct ip_event_got_ip_t{struct {uint32_t ip;}ip_info;};
inline const char* esp_ip4addr_ntoa(const uint32_t*,char* out,size_t n){std::snprintf(out,n,"192.0.2.1");return out;}
'''

TEST = r'''
#include "wifi_station.h"
#include "ssid_manager.h"
SsidManager::SsidManager()=default;
SsidManager::~SsidManager()=default;
void SsidManager::AddSsid(const std::string& ssid,const std::string& password){
 ++saves;ssid_list_={{ssid,password}};
}
int main(){
 {
 WifiStation station;station.Start();
 std::string name="example-net",password="test-password";
 assert(station.Connect(name,password));
 assert(radio_connects==0 && radio_configs==0 && saves==0); // UI never calls driver/NVS.
 name="changed";password="changed";
 assert(!station.Connect(name,password)); // One pending request, no duplicate submission.
 std::vector<WifiNetwork> networks;
 assert(station.ScanNetworks(&networks,true) && radio_scans==0); // Join cannot be interrupted by scanning.
 deliver(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,nullptr);
 assert(radio_connects==0); // A stale background scan cannot override explicit selection.
 pump();
 assert(radio_connects==1 && radio_configs==1 && radio_disconnects==1 && saves==1);
 assert(std::string(reinterpret_cast<char*>(last_config.sta.ssid))=="example-net");
 assert(std::string(reinterpret_cast<char*>(last_config.sta.password))=="test-password");
 // pump delivered the intentional disconnect too: it must not start a second join.
 ip_event_got_ip_t ip{};deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
 assert(station.IsConnected());
 deliver(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,nullptr);
 assert(radio_connects==1); // A healthy connection survives background scan completion.
 assert(station.Connect("example-net","test-password"));pump();
 assert(radio_connects==1 && saves==1); // Selecting the same connected network is harmless.
 wifi_event_sta_disconnected_t dropped{203};deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&dropped);
 assert(radio_connects==2); // A real disconnect still retries.
 fail_post=true;assert(!station.Connect("second-net","test-password"));fail_post=false;
 assert(station.Connect("second-net","test-password"));
 station.Stop();station.Start();
 assert(station.Connect("third-net","test-password"));pump();
 assert(radio_connects==3);
 assert(std::string(reinterpret_cast<char*>(last_config.sta.ssid))=="third-net"); // No stale request after stop/start.
 deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
 configure_result=ESP_FAIL;assert(station.Connect("failed-net","test-password"));pump();
 configure_result=ESP_OK;assert(station.Connect("recovery-net","test-password"));pump();
 assert(radio_connects==4); // Backend failure releases admission for another request.
 assert(!station.Connect("", "password"));
 assert(!station.Connect(std::string(33,'x'),"password"));
 }
 assert(registrations.empty());
 puts("Station: asynchronous admission, copied credentials, scan isolation, intentional disconnect, retries, cancellation and recovery passed");
}
'''


def main():
    component = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='pxa-wifi-test-') as temp:
        out = Path(temp)
        (out / 'fake.h').write_text(STUB)
        headers = ['esp_event.h', 'esp_timer.h', 'esp_netif.h', 'esp_wifi.h',
                   'esp_wifi_types_generic.h', 'esp_log.h', 'esp_system.h',
                   'nvs.h', 'nvs_flash.h', 'freertos/FreeRTOS.h', 'freertos/event_groups.h']
        for header in headers:
            target = out / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "fake.h"\n')
        (out / 'test.cpp').write_text(TEST)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-g',
                        '-fsanitize=address,undefined', '-I', str(out),
                        '-I', str(component / 'include'),
                        str(component / 'wifi_station.cc'), str(out / 'test.cpp'),
                        '-o', str(out / 'test')], check=True)
        subprocess.run([str(out / 'test')], check=True)


if __name__ == '__main__':
    main()
