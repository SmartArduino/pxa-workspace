#define _POSIX_C_SOURCE 200809L
#include "pxa_raster_handoff.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <errno.h>

static pxa_raster_handoff_t handoff;
static sem_t done;
static uint32_t stop, worker_jobs;
static struct { uint32_t generation; uint64_t input, output; } job;
static uint64_t expected(uint32_t generation, uint64_t input) {
    return (input ^ (UINT64_C(0x3d6356bc1921) * generation)) + 713;
}
static void execute(void) {
    uint32_t generation = job.generation;
    uint64_t input = job.input;
    if (generation % 7 == 0) sched_yield();
    assert(job.generation == generation && job.input == input);
    job.output = expected(generation, input);
}
static void *worker(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        if (pxa_raster_handoff_claim(&handoff)) {
            execute(); ++worker_jobs;
            assert(sem_post(&done) == 0);
        } else sched_yield();
    }
    return NULL;
}
int main(void) {
    pthread_t thread;
    unsigned stolen = 0;
    assert(sizeof(handoff) == 4);
    assert(!pxa_raster_handoff_claim(&handoff));
    assert(sem_init(&done, 0, 0) == 0);
    assert(pthread_create(&thread, NULL, worker, NULL) == 0);
    for (unsigned i = 1; i <= 100000; ++i) {
        job.generation = i; job.input = UINT64_C(0xabcdef91) * i; job.output = 0;
        pxa_raster_handoff_publish(&handoff);
        if (i % 3 == 0) sched_yield();
        if (pxa_raster_handoff_claim(&handoff)) { execute(); ++stolen; }
        else {
            int result;
            do { result = sem_wait(&done); } while (result && errno == EINTR);
            assert(result == 0);
        }
        assert(job.output == expected(i, job.input));
        assert(!pxa_raster_handoff_claim(&handoff)); /* Late/duplicate wake. */
    }
    __atomic_store_n(&stop, 1u, __ATOMIC_RELEASE);
    assert(pthread_join(thread, NULL) == 0);
    assert(stolen && worker_jobs && stolen + worker_jobs == 100000);
    assert(sem_destroy(&done) == 0);
    return 0;
}
