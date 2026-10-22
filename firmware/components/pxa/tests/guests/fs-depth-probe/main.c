#include "pxa.h"
#include "pxa_fs.h"

#define DEPTH 110u
static uint64_t token;
static unsigned stage, depth, path_size;
static int reading, finished;
static char path[256];
static uint8_t saved[] = "retained deep private save";

static int fail(const char *message) {
    finished = 1;
    (void)pxa_log_write(PXA_LOG_LEVEL_ERROR, message);
    return PXA_STATUS_INTERNAL;
}
static void initial_path(void) {
    path[0]='d'; path[1]='e'; path[2]='p'; path[3]='t'; path[4]='h'; path[5]=0;
    path_size=5; depth=0;
}
static void descend(void) {
    path[path_size++]='/'; path[path_size++]='n'; path[path_size]=0; ++depth;
}
static void save_path(void) {
    path[path_size++]='/'; path[path_size++]='s'; path[path_size++]='a';
    path[path_size++]='v'; path[path_size++]='e'; path[path_size]=0;
}
static int make_directory(void) {
    return pxa_fs_request_path(PXA_FS_MAKE_DIRECTORY, ++token, path, path_size);
}
static int open_save(void) {
    stage=2;
    return pxa_fs_request_open(++token, path, path_size, reading ? PXA_FS_OPEN_READ :
        PXA_FS_OPEN_WRITE | PXA_FS_OPEN_CREATE | PXA_FS_OPEN_TRUNCATE);
}
int32_t pxa_app_start(const uint8_t *config, uint32_t length) {
    (void)config; (void)length;
    token=0; stage=0; finished=reading=0; initial_path();
    for (unsigned index=0; index<DEPTH; ++index) descend();
    save_path();
    return pxa_fs_request_path(PXA_FS_STAT, ++token, path, path_size);
}
int32_t pxa_app_on_event(const uint8_t *bytes, uint32_t length) {
    pxa_event_t event;
    if (!pxa_parse_event(bytes, length, &event)) return PXA_STATUS_PROTOCOL_ERROR;
    if (finished || event.service!=PXA_FS_SERVICE || event.token!=token) return PXA_EVENT_UNHANDLED;
    if (stage==0) {
        pxa_fs_stat_result_t result;
        if (!pxa_fs_parse_stat(&event, token, &result)) return fail("FS DEPTH FAIL stat event");
        if (result.status==PXA_STATUS_OK) {
            if (result.kind!=PXA_FS_KIND_REGULAR || result.size!=sizeof(saved)-1u)
                return fail("FS DEPTH FAIL retained save size");
            reading=1;
            if (open_save()!=PXA_STATUS_OK) return fail("FS DEPTH FAIL read open request");
        } else if (result.status==PXA_STATUS_NOT_FOUND) {
            initial_path(); stage=1;
            if (make_directory()!=PXA_STATUS_OK) return fail("FS DEPTH FAIL mkdir request");
        } else return fail("FS DEPTH FAIL stat status");
    } else if (stage==1) {
        int32_t status;
        if (!pxa_fs_parse_status(&event, token, PXA_FS_MAKE_DIRECTORY, &status) ||
            (status!=PXA_STATUS_OK && status!=PXA_STATUS_BUSY)) return fail("FS DEPTH FAIL mkdir status");
        if (depth<DEPTH) {
            descend();
            if (make_directory()!=PXA_STATUS_OK) return fail("FS DEPTH FAIL nested request");
        } else {
            save_path();
            if (open_save()!=PXA_STATUS_OK) return fail("FS DEPTH FAIL write open request");
        }
    } else {
        pxa_fs_open_result_t result;
        if (!pxa_fs_parse_open(&event, token, &result) || result.status!=PXA_STATUS_OK)
            return fail("FS DEPTH FAIL open result");
        int good;
        if (reading) {
            uint8_t buffer[sizeof(saved)]={0};
            int32_t count=pxa_fs_read(result.handle, buffer, sizeof(buffer));
            good=count==(int32_t)sizeof(saved)-1;
            for (unsigned index=0; good && index<sizeof(saved)-1u; ++index)
                if (buffer[index]!=saved[index]) good=0;
            if (good && pxa_fs_read(result.handle, buffer, 1)!=0) good=0;
        } else good=pxa_fs_write(result.handle, saved, sizeof(saved)-1u)==(int32_t)sizeof(saved)-1;
        if (pxa_fs_close(result.handle)!=PXA_STATUS_OK || !good) return fail("FS DEPTH FAIL save content");
        finished=1;
        (void)pxa_log_write(PXA_LOG_LEVEL_INFO, reading ? "FS DEPTH PASS restored=1 depth=110" :
            "FS DEPTH PASS created=1 depth=110");
    }
    return PXA_EVENT_HANDLED;
}
void pxa_app_stop(uint32_t reason) {(void)reason;}
