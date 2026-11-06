// SPDX-License-Identifier: Apache-2.0
#include "ll_cam.h"
#include <assert.h>
#include <stdio.h>
static unsigned delegated;
bool __real_ll_cam_dma_sizes(cam_obj_t *camera) { (void)camera; ++delegated; return true; }
bool __wrap_ll_cam_dma_sizes(cam_obj_t *camera);
int main(void)
{
    const uint16_t sizes[][2]={{160,120},{320,240},{640,480},{800,600},{320,241}};
    for (unsigned i=0; i<sizeof(sizes)/sizeof(sizes[0]); ++i) {
        cam_obj_t camera={.width=sizes[i][0],.height=sizes[i][1],.in_bytes_per_pixel=2,.fb_bytes_per_pixel=2};
        assert(__wrap_ll_cam_dma_sizes(&camera));
        assert(camera.dma_buffer_size<=4096 && camera.dma_half_buffer_cnt==2);
        assert(camera.dma_node_buffer_size%4==0);
        assert(camera.dma_node_buffer_size==camera.dma_half_buffer_size);
        assert(camera.dma_buffer_size==camera.dma_node_buffer_size*2);
        assert(camera.dma_node_buffer_size%(camera.width*2)==0);
        assert((camera.width*camera.height*2)%camera.dma_half_buffer_size==0);
    }
    cam_obj_t invalid={.width=0,.height=240,.in_bytes_per_pixel=2,.fb_bytes_per_pixel=2};
    assert(!__wrap_ll_cam_dma_sizes(&invalid));
    invalid.width=320; invalid.height=0; assert(!__wrap_ll_cam_dma_sizes(&invalid));
    invalid.width=2048; invalid.height=240; assert(!__wrap_ll_cam_dma_sizes(&invalid));
    invalid.jpeg_mode=1; assert(__wrap_ll_cam_dma_sizes(&invalid) && delegated==1);
    invalid.jpeg_mode=0; invalid.psram_mode=true;
    assert(__wrap_ll_cam_dma_sizes(&invalid) && delegated==2);
    invalid.psram_mode=false; invalid.fb_bytes_per_pixel=1;
    assert(__wrap_ll_cam_dma_sizes(&invalid) && delegated==3);
    puts("PAI camera DMA: bounded ping-pong rings, complete rows/frames, odd dimensions and format isolation passed");
}
