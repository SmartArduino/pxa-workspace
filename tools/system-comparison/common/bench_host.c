/* SPDX-License-Identifier: MIT */
/* Reference checksums, not a desktop speed score. */
#include "bench_kernel.h"
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    for(unsigned kind=0;kind<5;kind++)printf("cpu %u %u\n",kind,bench_kernel(kind));
    uint16_t *pixels=calloc(296*240,2);if(!pixels)return 1;
    bench_paint(pixels,296,240,296,2,0);
    printf("pixels %u\n",bench_checksum(pixels,296,240,296));free(pixels);return 0;
}
