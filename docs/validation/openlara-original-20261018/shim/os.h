#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline char *__itoa(int x,char *s,int) {sprintf(s,"%d",x);return s;}
