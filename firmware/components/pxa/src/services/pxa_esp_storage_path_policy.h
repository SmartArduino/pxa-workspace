#ifndef PXA_ESP_STORAGE_PATH_POLICY_H
#define PXA_ESP_STORAGE_PATH_POLICY_H
#include <stdbool.h>
#include <string.h>
#include <strings.h>
static inline bool pxa_esp_storage_debug_child_writable(const char *child) {
    if(!child || !*child || strchr(child,'~') || child[strlen(child)-1]=='/') return false;
    for(const char *part=child; *part;) {
        const char *end=strchr(part,'/'); if(!end) end=part+strlen(part);
        if(end==part || end[-1]=='.' || end[-1]==' ') return false;
        part=*end?end+1:end;
    }
    /* Protect descendants and destructive operations on their ancestor. */
    return strcasecmp(child,"PXA") &&
        (strncasecmp(child,"PXA/system",10) || (child[10]!='/' && child[10]!='\0'));
}
#endif
