#include <pxa_raster.h>
#include <assert.h>

void pxa_sdk_benchmark_c_frame(uint8_t *bytes, uint64_t id) {
    pxa_raster_draw_list_t list;
    pxa_raster_draw_list_begin(&list, bytes, 4096, id);
    assert(pxa_raster_clear(&list, 0x001f));
    for (int i=0; i<128; ++i) {
        const int16_t x=(int16_t)((i%16)*256), y=(int16_t)((i/16)*256);
        const int16_t xy[8]={x,y,(int16_t)(x+128),y,(int16_t)(x+128),
            (int16_t)(y+128),x,(int16_t)(y+128)};
        assert(pxa_raster_flat_quad(&list, xy, (uint16_t)(i*31)));
    }
    assert(pxa_raster_submit(77, &list)==3112);
}
