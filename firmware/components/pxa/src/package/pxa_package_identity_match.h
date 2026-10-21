#ifndef PXA_PACKAGE_IDENTITY_MATCH_H
#define PXA_PACKAGE_IDENTITY_MATCH_H
#include <stddef.h>
#include <string.h>
static inline int pxa_package_identity_text_valid(const char *value) {
    if(!value)return 0;
    for(size_t i=0;i<64;++i)if(!((value[i]>='0' && value[i]<='9') || (value[i]>='a' && value[i]<='f')))return 0;
    if(value[64]!=':' || value[65]<'a' || value[65]>'z')return 0;
    size_t n=65;
    for(;n<130 && value[n];++n)if(!((value[n]>='a' && value[n]<='z') || (value[n]>='0' && value[n]<='9') || value[n]=='.' || value[n]=='_' || value[n]=='-'))return 0;
    return n<130;
}
/* Pure selection shared by Host management and Store launch. Directory order
 * never selects an owner. No allocation, I/O, or pointer survives a visit. */
typedef struct {
    const char *query;
    size_t matches;
    char identity[130];
    char app_id[65];
    unsigned char publisher_root[32];
} pxa_package_identity_match_t;
static inline void pxa_package_identity_consider(pxa_package_identity_match_t *m,
                                                 const char *identity, const char *app_id,
                                                 const unsigned char root[32]) {
    if (!m || !m->query || !identity || !app_id || !root ||
        (strcmp(m->query, identity) && strcmp(m->query, app_id)))
        return;
    if (strlen(identity) >= sizeof(m->identity) || strlen(app_id) >= sizeof(m->app_id))
        return;
    ++m->matches;
    if (m->matches == 1) {
        memcpy(m->identity, identity, strlen(identity) + 1);
        memcpy(m->app_id, app_id, strlen(app_id) + 1);
        memcpy(m->publisher_root, root, 32);
    } else {
        /* Do not expose a first candidate if a caller forgets to check matches. */
        m->identity[0] = m->app_id[0] = 0;
        memset(m->publisher_root, 0, 32);
    }
}
#endif
