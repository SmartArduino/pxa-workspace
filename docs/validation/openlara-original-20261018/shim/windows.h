#pragma once
#include "os.h"
#define DebugBreak() __builtin_trap()
static inline char *_itoa(int x,char*s,int b){return __itoa(x,s,b);}
