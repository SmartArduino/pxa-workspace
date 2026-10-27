#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_fs_roots.h"
#include "pxa_esp_storage_path_policy.h"
#include "pxa_esp_media_path.h"
#include "pxa_esp_volumes.h"
#include "pxa/pxa_esp_storage.h"
#include "pxa_esp_ui_shell.h"
#include "pxa/pxa_esp_surface.h"
#include "sdkconfig.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/md.h"
#include "pxa/wire.h"
#include "pxa/posix/pxa_file_browser.h"
#ifdef PXA_FS_IMPORT_LVGL
#include "pxa/lvgl/pxa_lvgl_file_picker.h"
#include "pxa/posix/pxa_import_destination.h"
#include "pxa_esp_dialog_style.h"
#include "pxa_esp_system_overlay.h"
#endif
#include "pxa/esp/pxa_esp_posix_shim.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_GRANTS 4u
#define GRANT_RECORD_BYTES 304u
#define GRANT_FILE_BYTES (16u+MAX_GRANTS*GRANT_RECORD_BYTES+32u)
static const char *const protected_paths[]={"PXA/apps","PXA/system","PXA/cache",CONFIG_PXA_STATE_ROOT};
#define g_volumes (pxa_esp_volumes_registry())
#define g_mounts (pxa_esp_volumes_mounts())
static uint64_t g_app_media_epoch;
static uint8_t g_activated;
static pthread_mutex_t g_choice_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t prompt_id; uint64_t epoch; uint8_t ready;
    char relative[PXA_ESP_STORAGE_PATH_BYTES];
    pxa_esp_fs_roots_t *owner;
    void *notify_context; void (*notify)(void *); } g_choice;
static uint32_t g_prompt_sequence;
typedef struct esp_file_import esp_file_import_t;
struct pxa_esp_fs_roots {
    esp_file_import_t *import;
    pxa_scoped_fs_t *fs;
    pxa_fs_roots_backend_t base,api;
    pxa_fs_service_t *service;
    pxa_scoped_fs_grant_t grants[MAX_GRANTS+1];
    char media_relative[256];
    char identity[130];
    char store_path[PXA_POSIX_APP_PATH_BYTES+32],app_name[64];
    pxa_esp_app_storage_t *app_storage;
    uint64_t media_mount_epoch;
    uint8_t media_retained;
    void *notify_context; void (*notify)(void *);
    pxa_component_t component; pxa_request_token_t token;
    uint32_t prompt_id,rights;
    uint8_t persistent;
    uint8_t persisted_mask;
    uint64_t next_grant_revision;
};
static int digest(const void *p,size_t n,uint8_t out[32]) {
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md && mbedtls_md(md,p,n,out)==0;
}
static pxa_status_t initialize_volumes(void) {
    return pxa_esp_volumes_initialize();
}
static const pxa_scoped_fs_mount_t *mount_for(uint64_t epoch) {
    uint16_t i;
    if(!epoch)return NULL;
    for(i=0;i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) if(g_mounts[i].mount_epoch==epoch) return &g_mounts[i];
    return NULL;
}
static pxa_status_t select_app_media(const char *path) {
    if(g_activated || g_app_media_epoch) return PXA_STATUS_BAD_STATE;
    if(!path) return PXA_STATUS_INVALID_ARGUMENT;
    for(uint16_t i=0;i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) if(g_mounts[i].host_path && !strcmp(path,g_mounts[i].host_path)) {
        pxa_volume_info_t v;
        if(pxa_volume_get(g_volumes,g_mounts[i].mount_epoch,&v)!=PXA_STATUS_OK ||
            v.ambiguous || v.state!=PXA_VOLUME_READY) return PXA_STATUS_UNAVAILABLE;
        g_app_media_epoch=g_mounts[i].mount_epoch;
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_NOT_FOUND;
}
static pxa_status_t media_mount(char *output,size_t capacity) {
    const pxa_scoped_fs_mount_t *m=mount_for(g_app_media_epoch);
    pxa_volume_info_t v;
    if(!output || !capacity)return PXA_STATUS_INVALID_ARGUMENT;
    output[0]=0;
    if(!m || pxa_volume_get(g_volumes,g_app_media_epoch,&v)!=PXA_STATUS_OK ||
        v.ambiguous || v.state!=PXA_VOLUME_READY)return PXA_STATUS_UNAVAILABLE;
    size_t prefix=strlen(CONFIG_PXA_MOUNT_POINT);
    if(!strcmp(m->host_path,CONFIG_PXA_MOUNT_POINT))return PXA_STATUS_OK;
    if(strncmp(m->host_path,CONFIG_PXA_MOUNT_POINT,prefix) ||
        m->host_path[prefix]!='/')return PXA_STATUS_DENIED;
    int n=snprintf(output,capacity,"%s",m->host_path+prefix+1);
    return n<0 || (size_t)n>=capacity?PXA_STATUS_RESOURCE_LIMIT:PXA_STATUS_OK;
}
static bool debug_path(const char *relative,bool system_unlocked,char *temporary,size_t capacity) {
    const char *base=CONFIG_PXA_MOUNT_POINT;
    if(!relative || !pxa_fs_scoped_path_is_valid(
        (pxa_bytes_t){(const uint8_t *)relative,strlen(relative)})) return false;
    /* The optional FAT replacement mode deletes an old destination before
     * renaming. This data transfer profile requires no-replace semantics. */
#ifdef CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION
    return false;
#endif
    char absolute[2*PXA_ESP_STORAGE_PATH_BYTES],mount[PXA_ESP_STORAGE_PATH_BYTES];pxa_volume_info_t volume;
    int n=snprintf(absolute,sizeof(absolute),"%s/%s",base,relative);
    if(n<0 || (size_t)n>=sizeof(absolute) || pxa_esp_storage_resolve_volume(absolute,&volume)!=PXA_STATUS_OK ||
        !(volume.capabilities&PXA_VOLUME_WRITE) ||
        pxa_esp_storage_volume_path(volume.mount_epoch,mount,sizeof(mount))!=PXA_STATUS_OK)return false;
    /* Public media is importable on internal Flash too. Keep application
     * code/private state behind the primary filesystem's legacy policy. */
    if(!strcmp(mount,base) && (volume.capabilities&PXA_VOLUME_PRIVATE_NATIVE) &&
        !pxa_esp_storage_debug_primary_writable(relative,system_unlocked))return false;
    size_t prefix=strlen(mount);
    if(strncmp(absolute,mount,prefix) || absolute[prefix]!='/' || !absolute[prefix+1] ||
        !pxa_esp_storage_debug_child_writable(absolute+prefix+1,system_unlocked))return false;
    if(temporary) {
        n=snprintf(temporary,capacity,"%s/.pxa-pxadb-upload",mount);
        if(n<0 || (size_t)n>=capacity)return false;
    }
    return true;
}
static pxa_status_t folders(uint64_t epoch,const char *relative,
    uint32_t skip,pxa_esp_storage_folder_t *out,size_t capacity,size_t *count,uint8_t *more) {
    char path[768]; DIR *directory; struct dirent *entry; uint32_t seen=0,scanned=0;
    const pxa_scoped_fs_mount_t *m=mount_for(epoch); pxa_volume_info_t v;
    if (!count || !more || !relative || !out || !capacity) return PXA_STATUS_INVALID_ARGUMENT;
    *count=0; *more=0;
    if (!m || pxa_volume_get(g_volumes,epoch,&v)!=PXA_STATUS_OK || v.ambiguous ||
        v.state!=PXA_VOLUME_READY) return PXA_STATUS_UNAVAILABLE;
    if (strlen(relative)>=256 || strchr(relative,'~') || (*relative &&
        !pxa_fs_scoped_path_is_valid((pxa_bytes_t){(const uint8_t *)relative,strlen(relative)})))
        return PXA_STATUS_INVALID_ARGUMENT;
    int n=snprintf(path,sizeof(path),"%s%s%s",m->host_path,*relative?"/":"",relative);
    if(n<0 || (size_t)n>=sizeof(path)) return PXA_STATUS_LIMIT_EXCEEDED;
    directory=opendir(path); if(!directory) return PXA_STATUS_IO_ERROR;
    while(scanned++<4096 && (entry=readdir(directory))) {
        struct stat st; size_t len=strlen(entry->d_name);
        if (strchr(entry->d_name,'~') || !pxa_fs_scoped_path_is_valid(
            (pxa_bytes_t){(const uint8_t *)entry->d_name,len})) continue;
        n=snprintf(path,sizeof(path),"%s/%s%s%s",m->host_path,relative,*relative?"/":"",entry->d_name);
        if(n<0 || (size_t)n>=sizeof(path) || stat(path,&st)!=0 || !S_ISDIR(st.st_mode)) continue;
        if (seen++<skip) continue;
        if (*count==capacity) { *more=1; break; }
        if (len>=sizeof(out[0].name)) { closedir(directory); return PXA_STATUS_LIMIT_EXCEEDED; }
        strcpy(out[(*count)++].name,entry->d_name);
    }
    closedir(directory);
    return scanned>4096 ? PXA_STATUS_LIMIT_EXCEEDED : PXA_STATUS_OK;
}
pxa_status_t pxa_esp_storage_select_app_media(const char *path) {
    pxa_status_t status=initialize_volumes();if(status!=PXA_STATUS_OK)return status;
    pxa_esp_volumes_lock();status=select_app_media(path);pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_media_mount(char *output,size_t capacity) {
    pxa_status_t status;
    pxa_esp_volumes_lock();status=media_mount(output,capacity);pxa_esp_volumes_unlock();return status;
}
bool pxa_esp_storage_debug_path(const char *relative,bool unlocked,char *temporary,size_t capacity) {
    bool result;
    pxa_esp_volumes_lock();result=debug_path(relative,unlocked,temporary,capacity);pxa_esp_volumes_unlock();return result;
}
pxa_status_t pxa_esp_storage_folders(uint64_t epoch,const char *relative,
    uint32_t skip,pxa_esp_storage_folder_t *out,size_t capacity,size_t *count,uint8_t *more) {
    pxa_status_t status;
    pxa_esp_volumes_lock();status=folders(epoch,relative,skip,out,capacity,count,more);pxa_esp_volumes_unlock();return status;
}
static int save_grants(pxa_esp_fs_roots_t *r) {
    uint8_t encoded[GRANT_FILE_BYTES]={0}; char temporary[sizeof(r->store_path)+5];
    FILE *f; unsigned i; int ok;
    memcpy(encoded,r->app_storage?"PXAGRNT2":"PXAGRNT1",8); pxa_write_u16(encoded+8,MAX_GRANTS);
    for(i=0;i<MAX_GRANTS;++i) if(r->persisted_mask & (1u<<i)) {
        const pxa_scoped_fs_grant_t *g=&r->grants[i];
        uint8_t *p=encoded+16+i*GRANT_RECORD_BYTES; size_t n=strlen(g->relative_path);
        memcpy(p,g->id,16); memcpy(p+16,g->volume_id.bytes,16);
        pxa_write_u64(p+32,g->revision); pxa_write_u32(p+40,g->rights);
        p[44]=g->active; pxa_write_u16(p+46,(uint16_t)n);
        if(n>=GRANT_RECORD_BYTES-48)return 0;
        memcpy(p+48,g->relative_path,n);
    }
    if(r->app_storage) {
        if(pxa_esp_app_storage_seal_grants(r->app_storage,
            (pxa_bytes_t){encoded,sizeof(encoded)-32},encoded+sizeof(encoded)-32)!=PXA_STATUS_OK)return 0;
    } else if(!digest(encoded,sizeof(encoded)-32,encoded+sizeof(encoded)-32)) return 0;
    int n=snprintf(temporary,sizeof(temporary),"%s.tmp",r->store_path);
    if(n<0 || (size_t)n>=sizeof(temporary)) return 0;
    f=fopen(temporary,"wb"); if(!f) return 0;
    ok=fwrite(encoded,1,sizeof(encoded),f)==sizeof(encoded);
    if(ok && fflush(f)!=0) ok=0;
    if(ok && fsync(fileno(f))!=0) ok=0;
    if(fclose(f)!=0) ok=0;
    if(ok && rename(temporary,r->store_path)!=0) ok=0;
    if(ok && r->app_storage) {
        const char *root;int parent=-1;
        if(pxa_esp_app_storage_root(r->app_storage,&root)!=PXA_STATUS_OK ||
            (parent=pxa_esp_open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW))<0)ok=0;
        if(parent>=0) {
            if(pxa_esp_fsync(parent)!=0)ok=0;
            if(pxa_esp_close(parent)!=0)ok=0;
        }
    }
    if(!ok) unlink(temporary);
    return ok;
}
static void load_grants(pxa_esp_fs_roots_t *r) {
    uint8_t encoded[GRANT_FILE_BYTES],hash[32]; FILE *f=fopen(r->store_path,"rb");
    unsigned i,j; int bad=0;
    if(!f) return;
    size_t n=fread(encoded,1,sizeof(encoded),f);
    int extra=fgetc(f); fclose(f);
    if(n!=sizeof(encoded) || extra!=EOF || memcmp(encoded,r->app_storage?"PXAGRNT2":"PXAGRNT1",8) ||
        pxa_read_u16(encoded+8)!=MAX_GRANTS)return;
    if(r->app_storage) {
        if(pxa_esp_app_storage_verify_grants(r->app_storage,
            (pxa_bytes_t){encoded,sizeof(encoded)-32},encoded+sizeof(encoded)-32)!=PXA_STATUS_OK)return;
    } else if(!digest(encoded,sizeof(encoded)-32,hash) || memcmp(hash,encoded+sizeof(encoded)-32,32))return;
    for(i=0;i<MAX_GRANTS;++i) {
        const uint8_t *p=encoded+16+i*GRANT_RECORD_BYTES;
        pxa_scoped_fs_grant_t *g=&r->grants[i]; uint16_t len=pxa_read_u16(p+46);
        uint8_t id=0,volume=0;
        if(!p[44]) continue;
        if(p[44]!=1 || p[45] || len>=sizeof(g->relative_path) || len>=GRANT_RECORD_BYTES-48 ||
            pxa_read_u32(p+40)!=3 || !pxa_read_u64(p+32)) { bad=1; break; }
        for(j=0;j<16;++j) { id|=p[j]; volume|=p[16+j]; }
        if(!id || !volume || (len && !pxa_fs_scoped_path_is_valid(
            (pxa_bytes_t){p+48,len}))) { bad=1; break; }
        for(j=0;j<i;++j) if(r->grants[j].active && !memcmp(r->grants[j].id,p,16)) bad=1;
        if(bad) break;
        memcpy(g->id,p,16); memcpy(g->volume_id.bytes,p+16,16);
        g->revision=pxa_read_u64(p+32); g->rights=3; g->active=1;
        memcpy(g->relative_path,p+48,len); g->relative_path[len]=0;
        r->persisted_mask|=(uint8_t)(1u<<i);
    }
    if(bad) { memset(r->grants,0,sizeof(r->grants)); r->persisted_mask=0; }
}
static int create_folder(const char *path) {
    struct stat st;
    return (mkdir(path,0700)==0 || errno==EEXIST) &&
        stat(path,&st)==0 && S_ISDIR(st.st_mode);
}
static pxa_status_t app_media_grant(pxa_esp_fs_roots_t *r) {
    pxa_scoped_fs_grant_t *g=&r->grants[MAX_GRANTS];
    pxa_volume_info_t v; char path[384];
    const pxa_scoped_fs_mount_t *m=mount_for(r->media_mount_epoch);
    if(!m || pxa_volume_get(g_volumes,r->media_mount_epoch,&v)!=PXA_STATUS_OK ||
        v.ambiguous || v.state!=PXA_VOLUME_READY) return PXA_STATUS_UNAVAILABLE;
    if(g->active) return pxa_scoped_fs_validate_grant(r->fs,g);
    if(pxa_esp_media_relative_managed(r->media_relative) ||
        pxa_esp_app_storage_media_epoch(r->app_storage)) {
        /* Explicit bindings were checked by activation: signed generations
         * were authenticated; manual imports retain their original directory.
         * Missing media never becomes an empty replacement. */
        struct stat info;
        int n=snprintf(path,sizeof(path),"%s/%s",m->host_path,r->media_relative);
        if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
        if(stat(path,&info)!=0 || !S_ISDIR(info.st_mode))return PXA_STATUS_UNAVAILABLE;
    } else {
    /* These registered FAT mounts are Host-owned, link-free and contain no
     * nested mounts. Never accept a Guest-selected native path here. */
    int n=snprintf(path,sizeof(path),"%s/PXA",m->host_path);
    if(n<0 || (size_t)n>=sizeof(path) || !create_folder(path)) return PXA_STATUS_IO_ERROR;
    n=snprintf(path,sizeof(path),"%s/PXA/media",m->host_path);
    if(n<0 || (size_t)n>=sizeof(path) || !create_folder(path)) return PXA_STATUS_IO_ERROR;
    const char *nested=strchr(r->media_relative+10,'/');
    if(nested) {
        n=snprintf(path,sizeof(path),"%s/%.*s",m->host_path,(int)(nested-r->media_relative),r->media_relative);
        if(n<0 || (size_t)n>=sizeof(path) || !create_folder(path))return PXA_STATUS_IO_ERROR;
    }
    n=snprintf(path,sizeof(path),"%s/%s",m->host_path,r->media_relative);
    if(n<0 || (size_t)n>=sizeof(path) || !create_folder(path)) return PXA_STATUS_IO_ERROR;
    }
    for(unsigned attempt=0;attempt<8;++attempt) {
        uint8_t any=0; int duplicate=0; esp_fill_random(g->id,16);
        for(unsigned j=0;j<16;++j) any|=g->id[j];
        for(unsigned j=0;j<MAX_GRANTS;++j)
            if(r->grants[j].active && !memcmp(g->id,r->grants[j].id,16)) duplicate=1;
        if(any && !duplicate) {
            g->volume_id=v.id; g->revision=1; g->rights=3; g->active=1;
            strcpy(g->relative_path,r->media_relative);
            return pxa_scoped_fs_validate_grant(r->fs,g);
        }
    }
    return PXA_STATUS_INTERNAL;
}
static pxa_status_t app_open_root(void *c,uint8_t k,const uint8_t id[16],void **d,uint32_t *rights) {
    pxa_esp_fs_roots_t *r=c;
    pxa_status_t status;
    pxa_esp_volumes_lock();
    if(k==PXA_FS_ROOT_APP_MEDIA) {
        status=app_media_grant(r);
        if(status==PXA_STATUS_OK)status=r->base.open_root(r->base.context,PXA_FS_ROOT_GRANT,
            r->grants[MAX_GRANTS].id,d,rights);
    } else status=r->base.open_root(r->base.context,k,id,d,rights);
    pxa_esp_volumes_unlock();return status;
}
static pxa_status_t app_open_at(void *c,void *d,pxa_bytes_t p,uint32_t flags,
    void **f,pxa_fs_entry_t *i,uint64_t *features) {
    pxa_esp_fs_roots_t *r=c;
    return r->base.open_at(r->base.context,d,p,flags,f,i,features);
}
static pxa_status_t app_stat_at(void *c,void *d,pxa_bytes_t p,pxa_fs_entry_t *i) {
    pxa_esp_fs_roots_t *r=c; return r->base.stat_at(r->base.context,d,p,i);
}
static pxa_status_t app_query_root(void *c,void *d,pxa_fs_root_info_t *i) {
    pxa_esp_fs_roots_t *r=c; return r->base.query_root(r->base.context,d,i);
}
#ifdef PXA_FS_IMPORT_LVGL
#include "pxa_esp_file_import.inc"
#endif
int pxa_esp_fs_roots_back(pxa_esp_fs_roots_t *r) {
    if(!r)return 0;
#ifdef PXA_FS_IMPORT_LVGL
    if(import_back(r))return 1;
#endif
    if(!r->prompt_id)return 0;
    pxa_esp_ui_shell_back_directory_prompt(r->prompt_id);return 1;
}

pxa_status_t pxa_esp_storage_directory_entries(uint32_t id,uint64_t epoch,const char *path,
    size_t skip,size_t capacity,size_t *count,uint8_t *more,pxa_esp_directory_entry_fn emit,void *context) {
    if(!count || !more)return PXA_STATUS_INVALID_ARGUMENT;
    *count=0;*more=0;
    pthread_mutex_lock(&g_choice_lock);
    pxa_esp_fs_roots_t *r=g_choice.prompt_id==id && id?g_choice.owner:NULL;
    pxa_status_t status=r?pxa_file_browser_page(r->fs,&r->base.files,epoch,path,NULL,
        skip,capacity,count,more,emit,context):PXA_STATUS_CANCELLED;
    pthread_mutex_unlock(&g_choice_lock);return status;
}
pxa_status_t pxa_esp_storage_directory_check(uint32_t id,uint64_t epoch,const char *path) {
    if(!path || strlen(path)>=PXA_ESP_STORAGE_PATH_BYTES)return PXA_STATUS_LIMIT_EXCEEDED;
    pthread_mutex_lock(&g_choice_lock);
    pxa_esp_fs_roots_t *r=g_choice.prompt_id==id && id?g_choice.owner:NULL;
    pxa_volume_info_t volume;
    pxa_status_t status=r?pxa_esp_storage_volume(epoch,&volume):PXA_STATUS_CANCELLED;
    if(status==PXA_STATUS_OK && r->persistent && volume.identity_strength<PXA_VOLUME_ID_FILESYSTEM)
        status=PXA_STATUS_UNSUPPORTED;
    if(status==PXA_STATUS_OK) {
        pxa_scoped_fs_grant_t grant={.active=1,.rights=3,.revision=1};
        grant.id[0]=1;grant.volume_id=volume.id;strcpy(grant.relative_path,path);
        status=pxa_scoped_fs_validate_grant(r->fs,&grant);
    }
    pthread_mutex_unlock(&g_choice_lock);return status;
}

static void cancel_access(void *context,pxa_component_t component,pxa_request_token_t token) {
    pxa_esp_fs_roots_t *r=context;
    if(!r->prompt_id || component!=r->component || token!=r->token) return;
    pthread_mutex_lock(&g_choice_lock);
    if(g_choice.prompt_id==r->prompt_id) memset(&g_choice,0,sizeof(g_choice));
    pthread_mutex_unlock(&g_choice_lock);
    pxa_esp_ui_shell_dismiss_directory_prompt(r->prompt_id);
    pxa_esp_surface_runtime_modal_leave();
    r->prompt_id=0; r->token=0;
}
static pxa_status_t request_access(void *context,pxa_component_t c,
    pxa_request_token_t t,pxa_bytes_t purpose,uint32_t rights,uint8_t persistent) {
    pxa_esp_fs_roots_t *r=context; pxa_esp_ui_directory_prompt_t prompt={0};
    if(rights!=3) return PXA_STATUS_UNSUPPORTED;
    if(r->prompt_id) return PXA_STATUS_BUSY;
    if(purpose.size>=sizeof(prompt.purpose)) return PXA_STATUS_LIMIT_EXCEEDED;
    pxa_volume_info_t first;
    if(!pxa_esp_storage_volumes(&first,1)) return PXA_STATUS_UNAVAILABLE;
    strcpy(prompt.app_name,r->app_name); memcpy(prompt.purpose,purpose.data,purpose.size);
    pthread_mutex_lock(&g_choice_lock);
    if(g_choice.prompt_id) {pthread_mutex_unlock(&g_choice_lock);return PXA_STATUS_BUSY;}
    prompt.prompt_id=++g_prompt_sequence;
    if(!prompt.prompt_id) prompt.prompt_id=++g_prompt_sequence;
    r->prompt_id=prompt.prompt_id; r->component=c; r->token=t;
    r->persistent=persistent; r->rights=rights;
    memset(&g_choice,0,sizeof(g_choice)); g_choice.prompt_id=prompt.prompt_id;g_choice.owner=r;
    g_choice.notify=r->notify; g_choice.notify_context=r->notify_context;
    pthread_mutex_unlock(&g_choice_lock);
    pxa_esp_surface_runtime_modal_enter();
    if(!pxa_esp_ui_shell_post_directory_prompt(&prompt)) {
        cancel_access(r,c,t); return PXA_STATUS_RESOURCE_LIMIT;
    }
    return PXA_STATUS_OK;
}
bool pxa_esp_storage_choose_directory(uint32_t prompt_id,uint64_t epoch,const char *relative) {
    bool accepted=false;
    if(!relative || strnlen(relative,256)>=256) return false;
    pthread_mutex_lock(&g_choice_lock);
    if(prompt_id && prompt_id==g_choice.prompt_id && !g_choice.ready) {
        g_choice.epoch=epoch; strcpy(g_choice.relative,relative); g_choice.ready=1;
        if(g_choice.notify) g_choice.notify(g_choice.notify_context);
        accepted=true;
    }
    pthread_mutex_unlock(&g_choice_lock); return accepted;
}
void pxa_esp_fs_roots_poll(pxa_esp_fs_roots_t *r) {
    uint64_t epoch=0; char relative[256]; uint8_t ready=0,id[16]={0};
    pxa_status_t status=PXA_STATUS_CANCELLED; unsigned i,slot=MAX_GRANTS;
    pxa_component_t component; pxa_request_token_t token;
    if(!r || !r->service) return;
    pxa_fs_service_poll(r->service);
#ifdef PXA_FS_IMPORT_LVGL
    poll_import(r);
#endif
    if(!r->prompt_id) return;
    pthread_mutex_lock(&g_choice_lock);
    if(g_choice.prompt_id==r->prompt_id && g_choice.ready) {
        epoch=g_choice.epoch; strcpy(relative,g_choice.relative); ready=1;
    }
    pthread_mutex_unlock(&g_choice_lock);
    if(!ready) return;
    component=r->component; token=r->token;
    if(epoch) {
        pxa_volume_info_t v;
        status=pxa_esp_storage_volume(epoch,&v);
        if(status==PXA_STATUS_OK && r->persistent && v.identity_strength<PXA_VOLUME_ID_FILESYSTEM)
            status=PXA_STATUS_UNSUPPORTED;
        if(status==PXA_STATUS_OK) {
            for(i=0;i<MAX_GRANTS;++i) {
                if(r->grants[i].active && !memcmp(r->grants[i].volume_id.bytes,v.id.bytes,16) &&
                    !strcmp(r->grants[i].relative_path,relative)) { slot=i; break; }
                if(!r->grants[i].active && slot==MAX_GRANTS) slot=i;
            }
            if(slot==MAX_GRANTS || r->next_grant_revision==UINT64_MAX) status=PXA_STATUS_RESOURCE_LIMIT;
            else {
                pxa_scoped_fs_grant_t old=r->grants[slot]; uint8_t old_mask=r->persisted_mask;
                pxa_scoped_fs_grant_t *g=&r->grants[slot];
                if(!g->active) {
                    unsigned attempt;
                    memset(g,0,sizeof(*g));
                    for(attempt=0;attempt<8;++attempt) {
                        uint8_t any=0; unsigned j,k; int duplicate=0;
                        esp_fill_random(g->id,16);
                        for(j=0;j<16;++j) any|=g->id[j];
                        for(k=0;k<MAX_GRANTS;++k) if(k!=slot && r->grants[k].active &&
                            !memcmp(r->grants[k].id,g->id,16)) duplicate=1;
                        if(any && !duplicate) break;
                    }
                    if(attempt==8) status=PXA_STATUS_INTERNAL;
                    g->revision=r->next_grant_revision++;
                }
                g->volume_id=v.id; g->rights=r->rights; g->active=1;
                strcpy(g->relative_path,relative);
                if(status==PXA_STATUS_OK) status=pxa_scoped_fs_validate_grant(r->fs,g);
                if(status==PXA_STATUS_OK && r->persistent) {
                    r->persisted_mask|=(uint8_t)(1u<<slot);
                    if(!save_grants(r)) status=PXA_STATUS_IO_ERROR;
                }
                if(status==PXA_STATUS_OK) memcpy(id,g->id,16);
                else { *g=old; r->persisted_mask=old_mask; }
            }
        }
    }
    cancel_access(r,component,token);
    (void)pxa_fs_access_complete(r->service,component,token,status,
        status==PXA_STATUS_OK?id:NULL);
}
static pxa_status_t app_media_path(void *context,char *out,size_t capacity,size_t *size) {
    pxa_esp_fs_roots_t *r=context;
    if(!r || !out || !size)return PXA_STATUS_INVALID_ARGUMENT;
    int n=snprintf(out,capacity,"/%s/",r->media_relative);
    if(n<0 || (size_t)n>=capacity)return PXA_STATUS_RESOURCE_LIMIT;
    *size=(size_t)n;return PXA_STATUS_OK;
}
static void enter_inventory(void *context) {(void)context;pxa_esp_volumes_lock();}
static void leave_inventory(void *context) {(void)context;pxa_esp_volumes_unlock();}
pxa_status_t pxa_esp_fs_roots_create(const pxa_esp_fs_roots_config_t *config,pxa_esp_fs_roots_t **out,
    pxa_fs_roots_backend_t **backend) {
    pxa_esp_fs_roots_t *r; pxa_scoped_fs_config_t c={0}; void *workspace;
    pxa_status_t status; unsigned i; char root[192]; int n;
    if(!config || !out || !backend || !config->allocate || !config->identity || strlen(config->identity)>129 ||
       !pxa_esp_media_relative_valid(config->media_relative))
        return PXA_STATUS_INVALID_ARGUMENT;
    *out=NULL; *backend=NULL;
    status=initialize_volumes(); if(status!=PXA_STATUS_OK) return status;
    g_activated=1;
    r=config->allocate(config->allocator_context,sizeof(*r)); if(!r) return PXA_STATUS_RESOURCE_LIMIT;
    memset(r,0,sizeof(*r)); r->notify_context=config->notify_context; r->notify=config->notify;
    r->app_storage=config->app_storage;
    r->media_mount_epoch=config->media_mount_epoch?config->media_mount_epoch:g_app_media_epoch;
    memcpy(r->identity,config->identity,strlen(config->identity)+1);
    strcpy(r->media_relative,config->media_relative);
    const char *display=config->name?config->name:config->identity;
    if(!strcmp(display,config->identity) && strchr(config->identity,':')) display=strchr(config->identity,':')+1;
    snprintf(r->app_name,sizeof(r->app_name),"%.63s",display);
    if(config->user_directories) {
        if(r->app_storage) {
            const char *private_root;
            status=pxa_esp_app_storage_root(r->app_storage,&private_root);if(status!=PXA_STATUS_OK)return status;
            n=snprintf(r->store_path,sizeof(r->store_path),"%s/.pxa-directory-grants",private_root);
        } else {
            n=snprintf(root,sizeof(root),"%s/%s/grants",CONFIG_PXA_MOUNT_POINT,CONFIG_PXA_STATE_ROOT);
            if(n<0 || (size_t)n>=sizeof(root) || !create_folder(root)) return PXA_STATUS_IO_ERROR;
            n=snprintf(r->store_path,sizeof(r->store_path),"%s/%s",root,config->identity);
        }
        if(n<0 || (size_t)n>=sizeof(r->store_path)) return PXA_STATUS_LIMIT_EXCEEDED;
        load_grants(r);
    }
    r->next_grant_revision=1;
    for(i=0;i<MAX_GRANTS;++i) if(r->grants[i].active && r->grants[i].revision>=r->next_grant_revision)
        r->next_grant_revision=r->grants[i].revision==UINT64_MAX?UINT64_MAX:r->grants[i].revision+1;
    c.struct_size=sizeof(c); c.volumes=g_volumes; c.mounts=g_mounts;
    c.mount_count=PXA_ESP_STORAGE_MAX_VOLUMES; c.grants=r->grants; c.grant_count=MAX_GRANTS+1;
    c.enter_inventory=enter_inventory;c.leave_inventory=leave_inventory;
    c.protected_paths=protected_paths;c.protected_path_count=sizeof(protected_paths)/sizeof(protected_paths[0]);
    c.max_open_resources=8;
    size_t bytes=pxa_scoped_fs_workspace_size(&c);
    workspace=config->allocate(config->allocator_context,bytes); if(!workspace) return PXA_STATUS_RESOURCE_LIMIT;
    pxa_esp_volumes_lock();status=pxa_scoped_fs_init(workspace,bytes,&c,&r->fs,&r->base);pxa_esp_volumes_unlock();
    if(status!=PXA_STATUS_OK) return status;
    r->api=r->base; r->api.context=r;
    r->api.open_root=app_open_root; r->api.open_at=app_open_at;
    r->api.stat_at=app_stat_at; r->api.query_root=app_query_root;
    /* read_at deliberately keeps files.context, unlike root callbacks which
     * use api.context. Copying the base callback preserves that contract. */
    r->api.request_access=config->user_directories?request_access:NULL;
    r->api.cancel_access=config->user_directories?cancel_access:NULL;
    r->api.app_media_path=app_media_path;
#ifdef PXA_FS_IMPORT_LVGL
    if(!pxa_esp_media_relative_managed(r->media_relative)) {
        r->api.request_import=request_import; r->api.cancel_import=cancel_import;
    }
#endif
    r->api.features=PXA_FS_FEATURE_APP_MEDIA_ROOT|PXA_FS_FEATURE_APP_MEDIA_PATH;
    if(r->media_mount_epoch) {
        status=pxa_esp_volumes_retain(r->media_mount_epoch);
        if(status!=PXA_STATUS_OK && config->media_mount_epoch) {pxa_scoped_fs_deinit(r->fs);return status;}
        if(status==PXA_STATUS_OK)r->media_retained=1;
    }
    *out=r; *backend=&r->api; return PXA_STATUS_OK;
}
void pxa_esp_fs_roots_bind(pxa_esp_fs_roots_t *r,pxa_fs_service_t *service) {
    if(r) r->service=service;
}
uint64_t pxa_esp_fs_roots_features(const pxa_esp_fs_roots_t *r) {
    if(!r) return 0;
    return PXA_FS_FEATURE_SCOPED_ROOTS | r->api.features |
        (r->api.read_at?PXA_FS_FEATURE_READ_AT:0) |
        (r->api.request_access?PXA_FS_FEATURE_USER_DIRECTORY_GRANTS:0) |
        (r->api.request_import?PXA_FS_FEATURE_FILE_IMPORT:0);
}
void pxa_esp_fs_roots_destroy(pxa_esp_fs_roots_t *r) {
    if(!r) return;
    cancel_access(r,r->component,r->token);
#ifdef PXA_FS_IMPORT_LVGL
    if(r->import)cancel_import(r,r->import->component,r->import->token);
#endif
    pxa_scoped_fs_deinit(r->fs); r->service=NULL;
    if(r->media_retained) {pxa_esp_volumes_release(r->media_mount_epoch);r->media_retained=0;}
}
