#ifndef PXA_ESP_MEDIA_PATH_H
#define PXA_ESP_MEDIA_PATH_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Only Host-bound publisher/app directories are media roots. A single
 * AppKey segment is not an alternate layout and must never be generated. */
static inline bool pxa_esp_media_relative_valid(const char *path) {
    if(!path || strncmp(path,"PXA/media/",10) || strlen(path)>=256)return false;
    const char *part=path+10;
    for(unsigned segment=0;segment<2;++segment) {
        const char *end=strchr(part,'/');
        size_t length=end?(size_t)(end-part):strlen(part);
        if(!length || length>64 || part[length-1]=='.' ||
           !((part[0]>='a' && part[0]<='z') || (part[0]>='0' && part[0]<='9')))
            return false;
        for(size_t i=0;i<length;++i)
            if(!((part[i]>='a' && part[i]<='z') || (part[i]>='0' && part[i]<='9') ||
                 part[i]=='-' || part[i]=='_' || part[i]=='.'))return false;
        if(segment==1) {
            if(!end)return true;
            const char *digest=end+1;
            if(strlen(digest)!=72 || strcmp(digest+64,"/package"))return false;
            for(unsigned i=0;i<64;++i)
                if(!((digest[i]>='0' && digest[i]<='9') || (digest[i]>='a' && digest[i]<='f')) ||
                    !((path[10+i]>='0' && path[10+i]<='9') || (path[10+i]>='a' && path[10+i]<='f')))return false;
            return part-path==75; /* Managed publisher is a full digest. */
        }
        if(!end)return false;
        part=end+1;
    }
    return false;
}
static inline bool pxa_esp_media_relative_managed(const char *path) {
    return pxa_esp_media_relative_valid(path) && strchr(strchr(path+10,'/')+1,'/')!=NULL;
}
#endif
