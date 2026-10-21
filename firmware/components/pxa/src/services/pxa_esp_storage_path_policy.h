#ifndef PXA_ESP_STORAGE_PATH_POLICY_H
#define PXA_ESP_STORAGE_PATH_POLICY_H
#include <stdbool.h>
#include <string.h>
#include <strings.h>
static inline bool pxa_esp_storage_debug_child_writable(const char *child,bool system_unlocked) {
    if(!child || !*child || child[strlen(child)-1]=='/') return false;
    for(const char *part=child; *part;) {
        const char *end=strchr(part,'/'); if(!end) end=part+strlen(part);
        if(end==part || end[-1]=='.' || end[-1]==' ') return false;
        part=*end?end+1:end;
    }
    /* Keep the system root and its ancestor in place, just like Flash system
     * roots. FSUNLOCK permits mutations below PXA/system. Reject FAT aliases
     * of this root; ordinary card paths and '~' filenames need no unlock. */
    if(!strcasecmp(child,"PXA")) return false;
    if(strncasecmp(child,"PXA/system",10)) return true;
    if(child[10]=='\0' || child[10]=='~') return false;
    return child[10]!='/' || system_unlocked;
}
#endif
