#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "pxa_locator_migration.h"
static pxa_app_locators_t durable;
static int commits, fail_commit, old_dir, next_dir, move_fails, moves,fail_after;
static const uint8_t root[32] = {9};
static pxa_status_t commit(void *c, uint64_t gen, const pxa_locator_publisher_t *p, size_t np,
                           const pxa_locator_app_t *a, size_t na) {
    (void)c;
    ++commits;
    if(commits==fail_commit && !fail_after)return PXA_STATUS_IO_ERROR;
    pxa_status_t status=pxa_app_locators_load(&durable,gen,p,np,a,na);
    return commits==fail_commit?PXA_STATUS_IO_ERROR:status;
}
static int probe(void *c, const char *path) {
    (void)c;
    return !strcmp(path, "old") ? old_dir : next_dir;
}
static pxa_status_t move(void *c, const char *a, const char *b) {
    (void)c;
    assert(!strcmp(a, "old") && !strcmp(b, "new"));
    ++moves;
    if (move_fails)
        return PXA_STATUS_IO_ERROR;
    assert(old_dir && !next_dir);
    old_dir = 0;
    next_dir = 1;
    return PXA_STATUS_OK;
}
static void reload(pxa_app_locators_t *r) {
    assert(pxa_app_locators_load(r, durable.generation, durable.publishers, durable.publisher_count,
                                 durable.apps, durable.app_count) == PXA_STATUS_OK);
}
int main(void) {
    pxa_locator_migration_io_t io = {NULL, probe, move};
    pxa_app_locators_t r;
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        pxa_app_locators_init(&r, NULL, commit);
        pxa_app_locators_init(&durable, NULL, commit);
        commits = fail_commit = moves = move_fails = fail_after = 0;
        old_dir = 1;
        next_dir = 0;
        assert(pxa_app_locators_reserve(&r, 1, root, "lucinhu", root, "pxa-nes", root, 7) ==
               PXA_STATUS_OK);
        if (scenario == 1)
            fail_commit = commits + 1; /* cannot persist intent: no move */
        if (scenario == 2)
            fail_commit = commits + 2; /* moved but completion not durable */
        if(scenario==7 || scenario==8){fail_after=1;fail_commit=commits+(scenario==7?1:2);}
        if (scenario == 3)
            move_fails = 1;
        if (scenario == 4)
            next_dir = 1; /* preserve both trees */
        if (scenario == 5) {
            old_dir = 0;
            next_dir = 1;
        } /* never adopt unknown new data */
        if (scenario == 6) {
            old_dir = 0;
            next_dir = 0;
        } /* no retained data */
        pxa_status_t status = pxa_locator_migrate(&r, root, "pxa-nes", 2, "old", "new", &io);
        if (scenario == 0 || scenario == 6)
            assert(status == PXA_STATUS_OK);
        else
            assert(status != PXA_STATUS_OK);
        if (scenario == 1 || (scenario >= 4 && scenario <= 7))
            assert(!moves);
        if (scenario == 4 || scenario == 5)
            continue;
        fail_commit = move_fails = 0;
        reload(&r);
        assert(pxa_locator_migrate(&r, root, "pxa-nes", 2, "old", "new", &io) == PXA_STATUS_OK);
        uint16_t flags;
        char relative[130];
        assert(pxa_app_locators_find(&r, root, "pxa-nes", &flags, relative, sizeof(relative)) ==
                   PXA_STATUS_OK &&
               flags == 5);
        if (scenario != 6)
            assert(!old_dir && next_dir);
    }
    puts("PASS: migration intent/rename/completion failures, recovery, empty sources and conflict "
         "preservation");
}
