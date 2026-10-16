#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {pthread_mutex_t lock;uint64_t next_operation;unsigned workers;void(*notify)(void*);void*context;void*slots[4];} Before;
typedef struct {pthread_mutex_t lock;uint64_t next_operation;unsigned workers;unsigned wake_pending;void(*notify)(void*);void*context;void*slots[4];} After;
int main(void){printf("pthread_mutex=%zu before=%zu after=%zu before_notify=%zu after_notify=%zu\n",sizeof(pthread_mutex_t),sizeof(Before),sizeof(After),offsetof(Before,notify),offsetof(After,notify));}
