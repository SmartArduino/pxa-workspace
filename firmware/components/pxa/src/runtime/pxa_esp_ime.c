#include "pxa_esp_ime.h"
#include "pxa_esp_package_store.h"
#include "pxa_esp_system_overlay.h"
#include "pxa/pxa_esp_surface.h"
#include "pxa/pxa_esp_system_font.h"
#include "pxa/wasi.h"
#include "pxa/wire.h"
#include "pxa/service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

/* UI fields are protected by lv_lock. Provider/runtime fields have one owner.
 * No dictionary, widget tree or service workspace exists before first focus. */
static struct {
    char *selected;
    pxa_lvgl_ime_t *keyboard;
    pxa_lvgl_ime_config_t ui;
    uint64_t generation;
    /* Keep flags together: a lone bool between uint64_t fields costs an
     * entire alignment slot on the ESP32. No extra storage for pending. */
    bool wanted, required, dirty, pending;
    pxa_runtime_t *runtime;
    pxa_wamr_engine_t *engine;
    pxa_component_engine_t operations;
    pxa_package_host_profile_t profile;
    pxa_posix_ime_provider_t *provider;
    pxa_ime_service_t *service;
    void *workspace;
    pxa_component_t component;
    uint64_t session, bound_generation;
    uint64_t submitted_at_us;
    pxa_host_input_method_timing_t timing;
    pxa_host_ime_input_command_t input;
    uint64_t (*next_instance)(void);
    int (*post)(const pxa_esp_host_command_t *);
    void (*wake)(void);
} ime;
static pxa_component_t watchdog_component = PXA_COMPONENT_INVALID;

static void focus_changed(void *context, uint64_t generation, bool active, bool required) {
    (void)context;
    ime.generation = generation; ime.wanted = active;
    ime.required = required; __atomic_store_n(&ime.dirty,true,__ATOMIC_RELEASE);
    if (ime.wake) ime.wake();
}
static pxa_status_t enqueue(void *context, uint64_t generation, uint16_t action,
    uint8_t layout, uint8_t language, uint32_t value, pxa_bytes_t text) {
    (void)context;
    if (!ime.post || text.size > sizeof(ime.input.text)) return PXA_STATUS_WOULD_BLOCK;
    pxa_esp_host_command_t command = {0}; command.type = PXA_ESP_HOST_CMD_IME_INPUT;
    command.payload.ime_input = (pxa_host_ime_input_command_t){
        .generation=generation, .queued_at_us=esp_timer_get_time(), .value=value, .action=action,
        .layout=layout, .language=language, .text_size=(uint8_t)text.size};
    if (text.size) memcpy(command.payload.ime_input.text,text.data,text.size);
    return ime.post(&command) ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}
static pxa_status_t present(void *context, const pxa_ime_frame_t *frame) {
    (void)context;
    lv_lock();
    if (frame && frame->sequence>1 && ime.submitted_at_us) {
        uint64_t processing=(uint64_t)esp_timer_get_time()-ime.submitted_at_us;
        uint64_t queued=ime.input.queued_at_us;
        ++ime.timing.count; ime.timing.queue_us+=queued; ime.timing.processing_us+=processing;
        if (queued>ime.timing.queue_max_us) ime.timing.queue_max_us=queued;
        if (processing>ime.timing.processing_max_us) ime.timing.processing_max_us=processing;
        ime.submitted_at_us=0;
    }
    pxa_status_t status = ime.keyboard ? pxa_lvgl_ime_present(
        ime.keyboard, ime.bound_generation, frame) : PXA_STATUS_BAD_STATE;
    lv_unlock(); pxa_esp_surface_require_composition();
    return status;
}
void pxa_esp_ime_initialize(uint64_t (*next_instance)(void),
    int (*post)(const pxa_esp_host_command_t *), void (*wake)(void)) {
    ime.next_instance=next_instance; ime.post=post; ime.wake=wake;
    ime.ui = (pxa_lvgl_ime_config_t){.struct_size=sizeof(ime.ui),.input=enqueue,.focus=focus_changed,
        .sized_text_font_path=pxa_esp_system_font_path()};
    nvs_handle_t handle; char selected[PXA_ESP_PACKAGE_ID_BYTES]; size_t size=sizeof(selected);
    if (nvs_open("pxa_ime",NVS_READONLY,&handle)==ESP_OK) {
        if (nvs_get_str(handle,"selected",selected,&size)==ESP_OK && *selected)
            ime.selected = strdup(selected);
        uint16_t packed=0;
        if (nvs_get_u16(handle,"preferences",&packed)==ESP_OK) {
            pxa_ime_preferences_t value={packed&255,packed>>8};
            if (pxa_ime_preferences_valid(value)) ime.ui.preferences=value;
        }
        nvs_close(handle);
    }
}
bool pxa_esp_ime_preferences(pxa_ime_preferences_t *value,bool save) {
    if (!value || (save && !pxa_ime_preferences_valid(*value))) return false;
    if (!save) { lv_lock(); *value=ime.ui.preferences; lv_unlock(); return true; }
    nvs_handle_t handle;
    if (nvs_open("pxa_ime",NVS_READWRITE,&handle)!=ESP_OK) return false;
    /* Defaults need no persistent record; restoring test settings also
     * restores the original namespace instead of keeping a redundant key. */
    esp_err_t status=(value->layout || value->language)
        ? nvs_set_u16(handle,"preferences",value->layout | (value->language<<8))
        : nvs_erase_key(handle,"preferences");
    if (status==ESP_ERR_NVS_NOT_FOUND) status=ESP_OK;
    if (status==ESP_OK) status=nvs_commit(handle);
    nvs_close(handle); if (status!=ESP_OK) return false;
    lv_lock(); ime.ui.preferences=*value;
    if (ime.keyboard) pxa_lvgl_ime_set_preferences(ime.keyboard,*value);
    lv_unlock(); return true;
}
bool pxa_esp_ime_selected(char *identity, size_t capacity) {
    if (!identity || !capacity) return false;
    lv_lock(); const char *selected = ime.selected ? ime.selected : "";
    bool fits = strlen(selected) < capacity;
    if (fits) memcpy(identity,selected,strlen(selected)+1);
    lv_unlock(); return fits;
}
bool pxa_esp_ime_select(const char *identity) {
    if (!identity || (*identity && !pxa_esp_package_store_is_input_method(identity))) return false;
    char *copy = *identity ? strdup(identity) : NULL;
    if (*identity && !copy) return false;
    nvs_handle_t handle;
    if (nvs_open("pxa_ime",NVS_READWRITE,&handle)!=ESP_OK) { free(copy); return false; }
    esp_err_t status = nvs_set_str(handle,"selected",identity);
    if (status==ESP_OK) status=nvs_commit(handle);
    nvs_close(handle);
    if (status!=ESP_OK) { free(copy); return false; }
    lv_lock(); free(ime.selected); ime.selected=copy;
    if (ime.keyboard) pxa_lvgl_ime_close(ime.keyboard);
    __atomic_store_n(&ime.dirty,true,__ATOMIC_RELEASE); lv_unlock();
    if (ime.wake) ime.wake();
    return true;
}
void pxa_esp_ime_attach(pxa_runtime_t *runtime, pxa_wamr_engine_t *engine,
    const pxa_component_engine_t *operations, const pxa_package_host_profile_t *profile,
    uint64_t (*next_instance)(void), int (*post)(const pxa_esp_host_command_t *), void (*wake)(void)) {
    lv_lock(); ime.runtime=runtime; ime.engine=engine; ime.operations=*operations; ime.profile=*profile;
    ime.next_instance=next_instance; ime.post=post; ime.wake=wake; lv_unlock();
}
void pxa_esp_ime_environment(const pxa_ui_environment_t *environment, const pxa_lvgl_ui_theme_t *theme) {
    lv_lock(); ime.ui.environment=*environment; ime.ui.theme=*theme;
    if (ime.keyboard) {
        pxa_lvgl_ime_set_theme(ime.keyboard,theme);
        pxa_lvgl_ime_set_environment(ime.keyboard,environment);
    }
    lv_unlock();
}
void pxa_esp_ime_editor_focus(void *context, void *textarea, bool focused) {
    (void)context;
    if (focused) {
        if (!ime.selected) return;
        if (!ime.keyboard) {
            ime.keyboard=pxa_lvgl_ime_create(&ime.ui);
            if (ime.keyboard) pxa_esp_system_overlay_add(pxa_lvgl_ime_root(ime.keyboard));
        }
        if (ime.keyboard) (void)pxa_lvgl_ime_focus(ime.keyboard,textarea);
    } else if (ime.keyboard && pxa_lvgl_ime_owns_textarea(textarea)) pxa_lvgl_ime_close(ime.keyboard);
}
bool pxa_esp_ime_managed(void *textarea) { return pxa_lvgl_ime_owns_textarea(textarea); }
void *pxa_esp_ime_navigation_root(void) {
    return pxa_lvgl_ime_active(ime.keyboard) ? pxa_lvgl_ime_root(ime.keyboard) : NULL;
}
bool pxa_esp_ime_dismiss(void) {
    if (!pxa_lvgl_ime_active(ime.keyboard)) return false;
    pxa_lvgl_ime_close(ime.keyboard);
    return true;
}
bool pxa_esp_ime_needs_runtime(void) {
    if (!__atomic_load_n(&ime.dirty,__ATOMIC_ACQUIRE)) return false;
    lv_lock(); bool needed=ime.selected && ime.wanted && ime.required &&
        pxa_lvgl_ime_loading_allowed(ime.keyboard); lv_unlock(); return needed;
}
bool pxa_esp_ime_active(void) {
    lv_lock(); bool active=pxa_lvgl_ime_active(ime.keyboard); lv_unlock(); return active;
}
void pxa_esp_ime_failed(void) {
    lv_lock(); ime.bound_generation=ime.generation; lv_unlock(); (void)present(NULL,NULL);
}
void pxa_esp_ime_suspend(void) {
    lv_lock(); if (ime.keyboard) pxa_lvgl_ime_close(ime.keyboard); lv_unlock();
}
void pxa_esp_ime_stop(void) {
    __atomic_store_n(&watchdog_component,PXA_COMPONENT_INVALID,__ATOMIC_RELEASE);
    if (ime.session && ime.service) pxa_ime_end(ime.service,ime.session);
    ime.session=0; ime.pending=false; ime.component=PXA_COMPONENT_INVALID;
    if (ime.provider) { pxa_esp_package_store_close_ime(ime.provider); ime.provider=NULL; }
    ime.submitted_at_us=0;
}
void pxa_esp_ime_release(void) {
    pxa_esp_ime_stop();
    lv_lock();
    if (ime.keyboard) {
        pxa_esp_system_overlay_remove(pxa_lvgl_ime_root(ime.keyboard));
        pxa_lvgl_ime_destroy(ime.keyboard); ime.keyboard=NULL;
    }
    ime.wanted=false; __atomic_store_n(&ime.dirty,false,__ATOMIC_RELEASE); ime.runtime=NULL; ime.engine=NULL;
    lv_unlock(); free(ime.workspace); ime.workspace=NULL; ime.service=NULL;
}
pxa_status_t pxa_esp_ime_prepare(pxa_component_t component, uint64_t instance) {
    uint8_t config[20]; size_t size=0;
    pxa_status_t status=pxa_posix_ime_provider_prepare(ime.provider,instance,component,config,sizeof(config),&size);
    if (status==PXA_STATUS_OK) {
        __atomic_store_n(&watchdog_component,component,__ATOMIC_RELEASE);
        return pxa_wamr_engine_set_config(ime.engine,instance,(pxa_bytes_t){config,size});
    }
    return status;
}
void pxa_esp_ime_input(const pxa_host_ime_input_command_t *command) {
    if (command->generation!=ime.bound_generation || !ime.session || command->text_size>sizeof(command->text)) return;
    ime.input=*command; ime.pending=true;
}
bool pxa_esp_ime_loading(void) {
    return pxa_posix_ime_provider_loading(ime.provider) ||
        (ime.submitted_at_us && ime.component==PXA_COMPONENT_INVALID);
}
void pxa_esp_ime_pump(void) {
    /* Focus publishes dirty under LVGL's lock. No editor activity: no lock,
     * dictionary polling or Guest entry on the foreground game loop. */
    if (!__atomic_load_n(&ime.dirty,__ATOMIC_ACQUIRE) && !ime.pending && !pxa_esp_ime_loading()) return;
    if (!ime.runtime) {
        lv_lock();
        if (ime.keyboard && !pxa_lvgl_ime_active(ime.keyboard)) {
            pxa_esp_system_overlay_remove(pxa_lvgl_ime_root(ime.keyboard));
            pxa_lvgl_ime_destroy(ime.keyboard); ime.keyboard=NULL;
        }
        if (!ime.wanted || !ime.required) __atomic_store_n(&ime.dirty,false,__ATOMIC_RELEASE);
        lv_unlock(); return;
    }
    char selected[PXA_ESP_PACKAGE_ID_BYTES]={0};
    lv_lock(); bool dirty=ime.dirty, wanted=ime.wanted && ime.required;
    if (dirty && wanted && !pxa_lvgl_ime_loading_allowed(ime.keyboard)) { lv_unlock(); return; }
    uint64_t generation=ime.generation;
    if (dirty) {
        __atomic_store_n(&ime.dirty,false,__ATOMIC_RELEASE);
        if (ime.selected) memcpy(selected,ime.selected,strlen(ime.selected)+1);
    }
    lv_unlock();
    pxa_status_t status=PXA_STATUS_OK;
    if (dirty) {
        ime.pending=false;
        if (!wanted || !*selected) {
            pxa_esp_ime_stop();
            lv_lock();
            pxa_lvgl_ime_provider_released(ime.keyboard,generation);
            if (ime.keyboard && !pxa_lvgl_ime_active(ime.keyboard)) {
                pxa_esp_system_overlay_remove(pxa_lvgl_ime_root(ime.keyboard));
                pxa_lvgl_ime_destroy(ime.keyboard); ime.keyboard=NULL;
            }
            /* Destroy's close callback ran under this lock, so only its dirty
             * notification is consumed. A later focus must never be erased. */
            if (ime.generation==generation) __atomic_store_n(&ime.dirty,false,__ATOMIC_RELEASE);
            lv_unlock(); return;
        }
        if (!ime.service) {
            ime.workspace=malloc(pxa_ime_service_workspace_size());
            pxa_ime_config_t config={sizeof(config),NULL,present};
            status=ime.workspace ? pxa_ime_service_init(ime.workspace,pxa_ime_service_workspace_size(),
                ime.runtime,&config,&ime.service) : PXA_STATUS_RESOURCE_LIMIT;
            if (status==PXA_STATUS_OK) status=pxa_ime_service_register(ime.service);
            if (status!=PXA_STATUS_OK) {
                free(ime.workspace); ime.workspace=NULL; ime.service=NULL;
            }
        }
        if (ime.session) pxa_ime_end(ime.service,ime.session);
        ime.session=0; ime.bound_generation=generation;
        if (status==PXA_STATUS_OK && !ime.provider) {
            const pxa_package_service_capability_t services[]={
                {PXA_SERVICE_CORE,{PXA_CORE_VERSION_MAJOR,PXA_CORE_VERSION_MINOR},0},
                {PXA_WASI_SERVICE_ID,{PXA_WASI_SERVICE_MAJOR,PXA_WASI_SERVICE_MINOR},PXA_WASI_FEATURE_CLOCKS},
                {PXA_IME_SERVICE_ID,{0,1},PXA_IME_FEATURE_ALL}};
            pxa_package_activation_profile_t profile={0};
            profile.core_version=(pxa_package_version_t){PXA_CORE_VERSION_MAJOR,PXA_CORE_VERSION_MINOR};
            profile.services=services; profile.service_count=3;
            if (!ime.submitted_at_us) {
                lv_lock(); memset(&ime.timing,0,sizeof(ime.timing)); lv_unlock();
                ime.submitted_at_us=esp_timer_get_time();
            }
            int64_t begin=esp_timer_get_time();
            status=pxa_esp_package_store_begin_ime(selected,ime.runtime,&ime.operations,&profile,
                &ime.profile,&ime.provider);
            uint64_t elapsed=esp_timer_get_time()-begin;
            lv_lock(); ++ime.timing.provider_steps;
            if (elapsed>ime.timing.provider_step_max_us) ime.timing.provider_step_max_us=(uint32_t)elapsed;
            if (status==PXA_STATUS_WOULD_BLOCK && ime.generation==generation && ime.wanted && ime.required)
                __atomic_store_n(&ime.dirty,true,__ATOMIC_RELEASE);
            lv_unlock();
            if (status==PXA_STATUS_OK || status==PXA_STATUS_WOULD_BLOCK) return;
        }
    }
    if (status==PXA_STATUS_OK && pxa_posix_ime_provider_loading(ime.provider)) {
        int64_t step=esp_timer_get_time();
        status=pxa_esp_package_store_step_ime(ime.provider,16384,ime.next_instance,&ime.component);
        uint64_t elapsed=esp_timer_get_time()-step;
        lv_lock(); ++ime.timing.provider_steps;
        if (elapsed>ime.timing.provider_step_max_us) ime.timing.provider_step_max_us=(uint32_t)elapsed;
        lv_unlock();
        if (status==PXA_STATUS_WOULD_BLOCK) return;
        lv_lock(); ime.timing.provider_load_us=esp_timer_get_time()-ime.submitted_at_us; lv_unlock();
        ime.submitted_at_us=0;
        ESP_LOGI("PxaIme","load status=%d us=%llu step_max=%u metadata=%u",status,
            (unsigned long long)ime.timing.provider_load_us,(unsigned)ime.timing.provider_step_max_us,
            (unsigned)pxa_posix_ime_provider_metadata_bytes(ime.provider));
    }
    if (status==PXA_STATUS_OK && ime.provider && !ime.session) {
        status=pxa_ime_begin(ime.service,ime.component,0,&ime.session);
        if (status==PXA_STATUS_OK) status=pxa_ime_send(ime.service,ime.session,PXA_IME_RESET,0,0,0,(pxa_bytes_t){0});
    }
    if (status!=PXA_STATUS_OK) { (void)present(NULL,NULL); pxa_esp_ime_stop(); return; }
    if (ime.pending && ime.session) {
        uint64_t sent=esp_timer_get_time();
        status=pxa_ime_send(ime.service,ime.session,ime.input.action,ime.input.layout,
            ime.input.language,ime.input.value,(pxa_bytes_t){ime.input.text,ime.input.text_size});
        if (status!=PXA_STATUS_WOULD_BLOCK) {
            ime.pending=false;
            if (status==PXA_STATUS_OK) {
                ime.submitted_at_us=sent;
                ime.input.queued_at_us=sent-ime.input.queued_at_us;
            }
        }
    }
}
void pxa_esp_ime_drain(void) {
    if (!ime.provider || !ime.runtime) return;
    pxa_wamr_event_result_t result;
    pxa_status_t status=pxa_wamr_engine_deliver_event_result(ime.engine,ime.runtime,ime.component,&result);
    if (status!=PXA_STATUS_OK && status!=PXA_STATUS_WOULD_BLOCK && status!=PXA_STATUS_NOT_FOUND &&
        !(status==PXA_STATUS_RESOURCE_LIMIT && !result.event_consumed)) {
        /* Provider faults are isolated from the foreground application's life. */
        (void)present(NULL,NULL); pxa_esp_ime_stop();
    }
}

void pxa_esp_ime_timing(pxa_host_input_method_timing_t *output) {
    if (!output) return;
    lv_lock(); *output=ime.timing;
    pxa_lvgl_ime_timing_t ui; pxa_lvgl_ime_timing(ime.keyboard,&ui);
    output->first_draw_ms=ui.first_draw_ms; output->candidates_ready_ms=ui.ready_ms;
    output->keyboard_drawn=ui.drawn; output->candidates_ready=ui.ready;
    lv_unlock();
}
bool pxa_esp_ime_poll_deadline(uint64_t now_us) {
    pxa_component_t component=__atomic_load_n(&watchdog_component,__ATOMIC_ACQUIRE);
    return component!=PXA_COMPONENT_INVALID &&
        pxa_wamr_engine_poll_component_deadline(ime.engine,component,now_us)!=0;
}
