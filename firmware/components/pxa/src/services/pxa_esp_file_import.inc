/* SPDX-License-Identifier: MIT
 * Trusted picker and bounded copy owner. Included by pxa_esp_fs_roots.c.
 * LVGL state is guarded by lv_lock; copy jobs run on the runtime owner.
 */
struct esp_file_import {
    pxa_lvgl_file_picker_t *picker;
    pxa_file_import_t job;
    pxa_import_destination_t destination;
    uint8_t destination_open;
    pxa_component_t component;
    pxa_request_token_t token;
    char root[PXA_FS_SCOPED_MAX_PATH_BYTES+1];
    uint8_t started;
};
static pxa_status_t import_emit(void *context,size_t index,uint8_t kind,pxa_bytes_t name) {
    pxa_file_picker_entry_t *rows=context;
    if(name.size>PXA_FS_SCOPED_MAX_PATH_BYTES)return PXA_STATUS_LIMIT_EXCEEDED;
    rows[index].name=malloc(name.size+1);if(!rows[index].name)return PXA_STATUS_RESOURCE_LIMIT;
    rows[index].kind=kind;rows[index].epoch=0;
    memcpy(rows[index].name,name.data,name.size);rows[index].name[name.size]=0;
    return PXA_STATUS_OK;
}
static pxa_status_t import_browse(void *context,uint64_t epoch,const char *relative,const char *extensions,
    size_t skip,pxa_file_picker_entry_t *rows,size_t capacity,size_t *count,uint8_t *more) {
    pxa_esp_fs_roots_t *r=context;
    if(epoch)return pxa_file_browser_page(r->fs,&r->base.files,epoch,relative,extensions,
        skip,capacity,count,more,import_emit,rows);
    pxa_volume_info_t volumes[PXA_ESP_STORAGE_MAX_VOLUMES];size_t seen=0;
    size_t n=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);*count=0;*more=0;
    for(size_t i=0;i<n;++i) {
        if(volumes[i].state!=PXA_VOLUME_READY || volumes[i].ambiguous || !(volumes[i].capabilities&PXA_VOLUME_READ))continue;
        if(seen++<skip)continue;
        if(*count==capacity) {*more=1;break;}
        pxa_file_picker_entry_t *row=&rows[(*count)++];row->epoch=volumes[i].mount_epoch;row->kind=PXA_FS_KIND_DIRECTORY;
        row->name=strdup(volumes[i].name);if(!row->name) {--*count;return PXA_STATUS_RESOURCE_LIMIT;}
    }
    return PXA_STATUS_OK;
}
static void import_overlay(lv_obj_t *object,int action) {
    if(action>0)pxa_esp_system_overlay_add(object);
    else if(action<0)pxa_esp_system_overlay_remove(object);
    else pxa_esp_system_overlay_refresh();
}
static void import_changed(void *context) {
    pxa_esp_fs_roots_t *r=context;if(r->notify)r->notify(r->notify_context);
}
static pxa_status_t request_import(void *context,pxa_component_t component,pxa_request_token_t token,
    uint32_t flags,pxa_bytes_t extensions,pxa_bytes_t purpose) {
    pxa_esp_fs_roots_t *r=context;
    if(r->import || r->prompt_id)return PXA_STATUS_BUSY;
    /* Authenticated media generations are immutable. Never import over assets
     * whose signatures were verified during installation. */
    if(pxa_esp_media_relative_managed(r->media_relative))return PXA_STATUS_UNSUPPORTED;
#ifdef CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION
    return PXA_STATUS_UNSUPPORTED;
#endif
    esp_file_import_t *f=calloc(1,sizeof(*f));if(!f)return PXA_STATUS_RESOURCE_LIMIT;
    char filter[PXA_FS_IMPORT_FILTER_MAX+1],reason[PXA_FS_IMPORT_PURPOSE_MAX+1];
    memcpy(filter,extensions.data,extensions.size);filter[extensions.size]=0;
    memcpy(reason,purpose.data,purpose.size);reason[purpose.size]=0;
    f->component=component;f->token=token;r->import=f;
    pxa_esp_surface_runtime_modal_enter();
    lv_lock();
    pxa_lvgl_file_picker_config_t config={
        .context=r,.browse=import_browse,.changed=import_changed,.overlay=import_overlay,
        .style=pxa_esp_dialog_theme(),.body_font=pxa_esp_host_ui_body_font(),
        .title_font=pxa_esp_host_ui_title_font(),.caption_font=pxa_esp_host_ui_caption_font(),
        .app_name=r->app_name,.purpose=reason,.extensions=filter,
        .multiple=(flags&PXA_FS_IMPORT_MULTIPLE)!=0,.chinese=pxa_esp_host_locale_is_chinese()};
    f->picker=pxa_lvgl_file_picker_create(&config);
    lv_unlock();
    if(!f->picker) {pxa_esp_surface_runtime_modal_leave();r->import=NULL;free(f);return PXA_STATUS_RESOURCE_LIMIT;}
    return PXA_STATUS_OK;
}
static pxa_status_t import_open_source(void *context,const pxa_file_choice_t *choice,void **resource,pxa_fs_entry_t *entry) {
    pxa_esp_fs_roots_t *r=context;
    return pxa_scoped_fs_open_trusted(r->fs,choice->epoch,choice->path,PXA_FS_KIND_REGULAR,resource,entry);
}
static pxa_status_t import_destination_ready(void *context) {
    pxa_esp_fs_roots_t *r=context;pxa_volume_info_t volume;
    pxa_status_t status=pxa_esp_storage_volume(r->media_mount_epoch,&volume);
    if(status!=PXA_STATUS_OK)return status;
    return volume.state==PXA_VOLUME_READY && !volume.ambiguous && (volume.capabilities&PXA_VOLUME_WRITE)
        ?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;
}
static pxa_status_t import_flush(void *context,void *resource) {
    pxa_esp_fs_roots_t *r=context;return pxa_import_destination_flush(&r->import->destination,resource);
}
static int import_same_file(void *context,const pxa_file_choice_t *choice) {
    pxa_esp_fs_roots_t *r=context;pxa_volume_info_t volume;size_t n=strlen(r->media_relative);
    if(choice->epoch!=r->media_mount_epoch || strlen(choice->path)<=n || choice->path[n]!='/')return 0;
    if(pxa_esp_storage_volume(r->media_mount_epoch,&volume)!=PXA_STATUS_OK)return 0;
    int same=(volume.capabilities&PXA_VOLUME_CASE_INSENSITIVE)?!strncasecmp(choice->path,r->media_relative,n):!strncmp(choice->path,r->media_relative,n);
    return same && !strchr(choice->path+n+1,'/');
}
static pxa_status_t start_import(pxa_esp_fs_roots_t *r) {
    esp_file_import_t *f=r->import;pxa_status_t status=import_destination_ready(r);
    if(status!=PXA_STATUS_OK)return status;
    pxa_esp_volumes_lock();status=app_media_grant(r);pxa_esp_volumes_unlock();
    if(status!=PXA_STATUS_OK)return status;
    char mount[PXA_ESP_STORAGE_PATH_BYTES];
    status=pxa_esp_storage_volume_path(r->media_mount_epoch,mount,sizeof(mount));if(status)return status;
    int n=snprintf(f->root,sizeof(f->root),"%s/%s",mount,r->media_relative);
    if(n<0 || (size_t)n>=sizeof(f->root))return PXA_STATUS_LIMIT_EXCEEDED;
    pxa_file_import_config_t copy={.context=r,.source=r->base.files,.open_source=import_open_source,
        .destination_ready=import_destination_ready,.flush_destination=import_flush,.same_file=import_same_file};
    pxa_volume_info_t volume;
    status=pxa_esp_storage_volume(r->media_mount_epoch,&volume);if(status)return status;
    if(volume.capabilities&PXA_VOLUME_CASE_INSENSITIVE)copy.maximum_name_utf16_units=255;
    else copy.maximum_name_bytes=255;
    status=pxa_import_destination_open(&f->destination,f->root,&copy.destination);if(status)return status;
    f->destination_open=1;
    size_t count;lv_lock();const pxa_file_choice_t *choices=pxa_lvgl_file_picker_selection(f->picker,&count);lv_unlock();
    uint64_t nonce;esp_fill_random(&nonce,sizeof(nonce));if(!nonce)nonce=1;
    status=pxa_file_import_begin(&f->job,&copy,choices,count,nonce);
    if(status==PXA_STATUS_OK)f->started=1;
    return status;
}
static void cancel_import(void *context,pxa_component_t component,pxa_request_token_t token) {
    pxa_esp_fs_roots_t *r=context;esp_file_import_t *f=r->import;
    if(!f || component!=f->component || token!=f->token)return;
    if(f->started)pxa_file_import_cancel(&f->job);
    lv_lock();pxa_lvgl_file_picker_destroy(f->picker);lv_unlock();
    pxa_esp_surface_runtime_modal_leave();
    if(f->destination_open)pxa_import_destination_close(&f->destination);
    free(f);r->import=NULL;
}
static void poll_import(pxa_esp_fs_roots_t *r) {
    esp_file_import_t *f=r->import;if(!f)return;
    lv_lock();unsigned state=pxa_lvgl_file_picker_state(f->picker);lv_unlock();
    if(!state)return;
    pxa_status_t status=state==2?PXA_STATUS_CANCELLED:f->started?PXA_STATUS_OK:start_import(r);
    if(status==PXA_STATUS_OK)status=pxa_file_import_step(&f->job,4);
    if(status==PXA_STATUS_WOULD_BLOCK) {
        lv_lock();pxa_lvgl_file_picker_progress(f->picker,f->job.index,f->job.count,f->job.total_copied);lv_unlock();
        import_changed(r);return;
    }
    pxa_fs_import_summary_t result=f->job.result;
    pxa_component_t component=f->component;pxa_request_token_t token=f->token;
    cancel_import(r,component,token);
    (void)pxa_fs_import_complete(r->service,component,token,status,status==PXA_STATUS_OK?&result:NULL);
}
static int import_back(pxa_esp_fs_roots_t *r) {
    if(!r || !r->import)return 0;
    lv_lock();pxa_lvgl_file_picker_back(r->import->picker);lv_unlock();
    return 1;
}
