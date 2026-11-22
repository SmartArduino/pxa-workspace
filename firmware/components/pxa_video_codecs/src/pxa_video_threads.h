// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pthread.h>
struct PxaVideoThreadAllocators { const void *external,*internal; void *fault; };
void pxa_video_worker_error();
PxaVideoThreadAllocators pxa_video_thread_allocators();
void pxa_video_thread_allocators(PxaVideoThreadAllocators);
int pxa_video_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int pxa_video_pthread_join(pthread_t,void **);
