#ifndef PXA_ESP_STORAGE_PATH_POLICY_H
#define PXA_ESP_STORAGE_PATH_POLICY_H
#include <stdbool.h>
#include <string.h>
#include <strings.h>
/* Internal native storage keeps its existing tooling boundary. Public media
 * and explicitly unlocked system resources use the same layout on any volume. */
static inline bool pxa_esp_storage_debug_primary_writable(const char *child,bool system_unlocked) {
    if(!child)return false;
    if(!strncmp(child,"PXA/media",9) && (!child[9] || child[9]=='/'))return true;
    return system_unlocked && !strncmp(child,"PXA/system/",11) && child[11];
}
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
