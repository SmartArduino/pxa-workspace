// SPDX-License-Identifier: Apache-2.0
#include "../src/esp_mosaico_peripherals.cc"
#include <cstdlib>
#include <cstdio>
static pxa_peripherals_backend_t backend;
static void* allocation;
extern "C" pxa_status_t pxa_esp_resource_memory_initialize(void) { return PXA_STATUS_OK; }
extern "C" const pxa_memory_allocator_t* pxa_esp_device_resource_allocator(unsigned, unsigned) { return nullptr; }
extern "C" void* pxa_memory_allocate(const pxa_memory_allocator_t*,size_t bytes) { allocation=std::malloc(bytes); return allocation; }
extern "C" void pxa_memory_release(void* ptr) { std::free(ptr); }
extern "C" bool pxa_esp_peripherals_set_backend(const pxa_peripherals_backend_t* value) { backend=*value; return true; }
#include <chrono>
#include <thread>
template<typename F> static void Wait(F condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()) { assert(std::chrono::steady_clock::now()<deadline); std::this_thread::yield(); }
}
static void Finish(void* native) { Close(nullptr,native); Cleanup(*static_cast<Device*>(native)); }
static void Restore(unsigned index) { assert(ConfigureSelector(index)); g_slots[index].restoring=false; }
int main(int argc,char**) {
    assert(EspMosaicoInitializePeripherals(reinterpret_cast<void*>(1),argc>1?0x100:0x102,true));
    assert(backend.ready==Ready);
    assert(NextWait(1000,1000)==250 && NextWait(1100,1000)==150 && NextWait(1300,1000)==1);
    assert(NextWait(49,UINT32_MAX-100)==100); // Millisecond wrap preserves the scan deadline.
    void *device=nullptr,*gpio=nullptr;
    assert(Open(nullptr,1,3,3,100000,0x20,&device)==PXA_STATUS_OK);
    assert(add_calls[0x20]==0 && Ready(nullptr,device)==PXA_STATUS_WOULD_BLOCK && NextWait(1000,1000)==1);
    block_add_address=0x20;
    std::thread worker([&]{Prepare(*static_cast<Device*>(device));});
    Wait([]{return add_entered.load();});
    uint8_t data=0,reg=0; size_t bytes=1;
    assert(Ready(nullptr,device)==PXA_STATUS_WOULD_BLOCK);
    assert(Io(nullptr,device,PXA_IO_READ,&data,1)==PXA_STATUS_WOULD_BLOCK);
    assert(Start(nullptr,device,1,&reg,1,1)==PXA_STATUS_WOULD_BLOCK);
    // Driver setup is deliberately stalled. Host metadata and other port
    // reservations still return, without waiting on the native I2C driver.
    assert(Open(nullptr,1,16,2,1,0,&gpio)==PXA_STATUS_OK && !levels[53]);
    assert(Ready(nullptr,gpio)==PXA_STATUS_WOULD_BLOCK && !release_add);
    Finish(gpio); Prepare(*static_cast<Device*>(gpio)); assert(!resets[53]); Restore(0);
    release_add=true; worker.join(); block_add_address=-1;
    assert(Ready(nullptr,device)==PXA_STATUS_OK && add_calls[0x20]==1);
    Finish(device);
    // Closing an initialization in progress retains its state until the worker exits.
    assert(Open(nullptr,1,3,3,100000,0x20,&device)==PXA_STATUS_OK);
    block_add_address=0x20; add_entered=false; release_add=false;
    std::thread cancelled([&]{Prepare(*static_cast<Device*>(device));});
    Wait([]{return add_entered.load();}); Close(nullptr,device);
    assert(Ready(nullptr,device)==PXA_STATUS_UNAVAILABLE);
    release_add=true; cancelled.join(); block_add_address=-1; Cleanup(*static_cast<Device*>(device));
    assert(!static_cast<Device*>(device)->live);
    add_failure_address=0x20;
    assert(Open(nullptr,1,3,3,100000,0x20,&device)==PXA_STATUS_OK);
    Prepare(*static_cast<Device*>(device)); Cleanup(*static_cast<Device*>(device));
    assert(Ready(nullptr,device)==PXA_STATUS_IO_ERROR && static_cast<Device*>(device)->live);
    assert(Open(nullptr,2,3,3,100000,0x20,&gpio)==PXA_STATUS_BUSY);
    Finish(device); add_failure_address=-1;
    // An externally installed UART must never be reset by a failed queued open.
    uart_live=true; const auto before_tx=resets[58],before_rx=resets[59];
    assert(Open(nullptr,1,4,4,115200,0,&device)==PXA_STATUS_OK);
    Prepare(*static_cast<Device*>(device)); assert(Ready(nullptr,device)==PXA_STATUS_BUSY);
    Finish(device); assert(uart_live && !uart_deletes && resets[58]==before_tx && resets[59]==before_rx);
    uart_live=false; Restore(1);
    void *left=nullptr,*right=nullptr;
    assert(Open(nullptr,1,1,5,0,0,&left)==PXA_STATUS_OK && Open(nullptr,1,2,5,0,0,&right)==PXA_STATUS_OK);
    Prepare(*static_cast<Device*>(left)); Prepare(*static_cast<Device*>(right));
    assert(NextWait(1000,1000)==TickType_t(portMAX_DELAY));
    assert(Open(nullptr,1,3,3,100000,0x20,&device)==PXA_STATUS_OK && NextWait(1000,1000)==1);
    Prepare(*static_cast<Device*>(device)); assert(NextWait(1000,1000)==TickType_t(portMAX_DELAY));
    assert(Start(nullptr,device,2,&reg,1,1)==PXA_STATUS_OK && NextWait(1000,1000)==1);
    Transfer(*static_cast<Device*>(device)); assert(NextWait(1000,1000)==TickType_t(portMAX_DELAY));
    assert(Poll(nullptr,device,2,&data,&bytes)==PXA_STATUS_OK);
    Close(nullptr,device); assert(NextWait(1000,1000)==20); Cleanup(*static_cast<Device*>(device));
    Finish(left); Finish(right); assert(NextWait(1000,1000)==20);
    Restore(0); Restore(1); assert(NextWait(1000,1000)==250);
    std::free(allocation);
    puts("Mosaico async setup: Host stays responsive, cancellation retains state, idle sleeps follow deadlines");
}
