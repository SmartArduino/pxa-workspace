#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "../src/watcher_lcd_io.cc"
#include "../src/watcher_pxa_surface.cc"

namespace {
spi_device_interface_config_t device;
struct Pending { spi_transaction_t* transaction; std::vector<uint8_t> expected; };
std::deque<Pending> pending;
std::vector<uint8_t> wire, command;
int fail_after = -1, callbacks = 0;
bool dma_failure = false;
bool owned = false;
bool registered = false;
size_t bus_max_bytes = 64;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t, uint32_t) {
    assert(!owned); owned=true; return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t) { assert(owned); owned=false; }
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t** result, uint32_t) {
    assert(!pending.empty());
    auto next=std::move(pending.front()); pending.pop_front();
    // Detect scratch reuse before DMA has read it.
    assert(std::memcmp(next.transaction->tx_buffer,next.expected.data(),next.expected.size())==0);
    wire.insert(wire.end(),next.expected.begin(),next.expected.end());
    device.post_cb(next.transaction); *result=next.transaction;
    return dma_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t* transaction) {
    assert(owned && pending.empty());
    const auto* bytes=transaction->flags & SPI_TRANS_USE_TXDATA ?
        transaction->tx_data:static_cast<const uint8_t*>(transaction->tx_buffer);
    command.insert(command.end(),bytes,bytes+transaction->length/8);
    device.post_cb(transaction); return ESP_OK;
}
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t* transaction, uint32_t) {
    assert(owned && pending.size()<2);
    if(fail_after==0) return ESP_FAIL;
    if(fail_after>0) --fail_after;
    const auto* bytes=static_cast<const uint8_t*>(transaction->tx_buffer);
    const size_t size=transaction->length/8;
    assert(!(transaction->flags & SPI_TRANS_DMA_USE_PSRAM));
    pending.push_back({transaction,{bytes,bytes+size}}); return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t) { assert(pending.empty());registered=false;return ESP_OK; }
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t,size_t* size) {
    if(!registered) return ESP_ERR_INVALID_ARG;
    *size=bus_max_bytes;return ESP_OK;
}
esp_err_t spi_bus_add_device(spi_host_device_t,const spi_device_interface_config_t* config,spi_device_handle_t* handle) {
    device=*config;*handle=&device;registered=true;return ESP_OK;
}

int main() {
    esp_lcd_panel_io_spi_config_t config{};config.flags.quad_mode=1;
    esp_lcd_panel_io_handle_t io{};
    config.lcd_cmd_bits=8;
    assert(WatcherLcdIoCreate(2,config,&io)==ESP_ERR_INVALID_ARG);
    config.lcd_cmd_bits=32;
    assert(WatcherLcdIoCreate(2,config,&io)==ESP_OK);
    esp_lcd_panel_io_callbacks_t events{[](esp_lcd_panel_io_t*,void*,void*) { ++callbacks; return true; }};
    assert(io->register_event_callbacks(io,&events,nullptr)==ESP_OK);
    alignas(64) uint8_t source[212], scratch[128];
    for(size_t n=0;n<sizeof(source);++n)source[n]=n;
    assert(io->tx_color(io,0x32002c00,source,sizeof(source))==ESP_OK);
    assert(callbacks==1 && pending.empty()); // Every completion, including the last, was checked.
    assert(io->tx_param(io,-1,nullptr,0)==ESP_OK);
    assert(callbacks==1 && wire==std::vector<uint8_t>(source,source+sizeof(source)));
    assert((command==std::vector<uint8_t>{0x32,0,0x2c,0}));
    command.clear();
    const uint8_t parameters[]{1,2,3,4,5,6,7};
    assert(io->tx_param(io,-1,parameters,sizeof(parameters))==ESP_OK);
    assert(command==std::vector<uint8_t>(parameters,parameters+sizeof(parameters)));
    wire.clear(); command.clear();
    WatcherLcdIoSetPacking(io,[](void*,const void* src,void* dst,size_t offset,size_t bytes) {
        const auto* input=static_cast<const uint8_t*>(src)+offset;
        auto* output=static_cast<uint8_t*>(dst);
        for(size_t n=0;n<bytes;n+=2){output[n]=input[n+1];output[n+1]=input[n];}
    },nullptr,scratch,64);
    assert(io->tx_color(io,0x32002c00,source,sizeof(source))==ESP_OK);
    assert(io->tx_param(io,-1,nullptr,0)==ESP_OK);
    assert(callbacks==2 && wire.size()==sizeof(source));
    for(size_t n=0;n<wire.size();++n)assert(wire[n]==source[n^1]);
    // UI colors use the same fixed DMA blocks without pixel conversion.
    WatcherLcdIoSetPacking(io,nullptr,nullptr,scratch,64);
    wire.clear();
    assert(io->tx_color(io,0x32002c00,source,sizeof(source))==ESP_OK);
    assert(wire==std::vector<uint8_t>(source,source+sizeof(source)));
    assert(callbacks==3);
    fail_after=2; wire.clear();
    assert(io->tx_color(io,0x32002c00,source,sizeof(source))==ESP_FAIL);
    assert(pending.empty() && !owned && callbacks==3);
    fail_after=-1;
    dma_failure=true;
    assert(io->tx_color(io,0x32002c00,source,sizeof(source))==ESP_FAIL);
    assert(pending.empty() && !owned);
    dma_failure=false;
    assert(io->tx_param(io,-1,nullptr,0)==ESP_OK); // No phantom inflight count.
    WatcherLcdIoSetPacking(io,nullptr,nullptr,nullptr,0);
    assert(io->tx_color(io,0x32002c00,source+1,35)==ESP_OK);
    assert(io->del(io)==ESP_OK && callbacks==4);
    // Exercise the production native converter through the real transport,
    // rather than a hand-built visible=true Surface or a mock byte swap. The
    // original native callback skipped all pixels and replayed old SRAM bands.
    std::vector<uint16_t> frame(412*412);
    for(size_t y=0;y<412;++y)
        for(size_t x=0;x<412;++x)frame[y*412+x]=(y*73+x*13)^0xa55a;
    alignas(64) uint8_t dma[2*412*12*2];
    for(const size_t rows : {4u,5u,6u,8u,10u,12u}) {
        const size_t block=412*rows*2;
        bus_max_bytes=block;
        assert(WatcherLcdIoCreate(2,config,&io)==ESP_OK);
        std::memset(dma,0x5a,sizeof(dma));
        wire.clear();command.clear();
        WatcherLcdIoSetPacking(io,[](void*,const void* source,void* destination,
                                   size_t offset,size_t bytes) {
            watcher_pxa_surface::PackNativeRgb565Band(source,destination,offset,bytes);
        },nullptr,dma,block);
        assert(io->tx_color(io,0x32002c00,frame.data(),frame.size()*2)==ESP_OK);
        assert(wire.size()==frame.size()*2);
        for(size_t pixel=0;pixel<frame.size();++pixel) {
            assert(wire[pixel*2]==uint8_t(frame[pixel]>>8));
            assert(wire[pixel*2+1]==uint8_t(frame[pixel]));
        }
        assert(pending.empty() && !owned);
        assert(io->del(io)==ESP_OK);
    }
    std::puts("Watcher LCD IO: wire order, bounded chunks, ownership and error recovery passed");
}
