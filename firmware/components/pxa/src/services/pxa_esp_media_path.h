#ifndef PXA_ESP_MEDIA_PATH_H
#define PXA_ESP_MEDIA_PATH_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Only Host-bound publisher/app directories are media roots. A single
 * AppKey segment is not an alternate layout and must never be generated. */
static inline bool pxa_esp_media_relative_valid(const char *path) {
    if(!path || strncmp(path,"PXA/media/",10) || strlen(path)>=140)return false;
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
        if(segment==1)return end==NULL;
        if(!end)return false;
        part=end+1;
    }
    return false;
}
#endif
