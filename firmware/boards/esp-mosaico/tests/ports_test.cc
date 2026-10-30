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
// Model Host's asynchronous OPEN completion for the existing fault cases.
static pxa_status_t OpenPrepared(void* context, uint64_t owner, uint16_t port, uint16_t mode,
    uint32_t parameter, uint16_t address, void** output) {
    auto status = Open(context, owner, port, mode, parameter, address, output);
    if (status != PXA_STATUS_OK) return status;
    Prepare(*static_cast<Device*>(*output));
    status = Ready(context, *output);
    if (status != PXA_STATUS_OK) {
        Close(context, *output); Cleanup(*static_cast<Device*>(*output)); *output = nullptr;
    }
    return status;
}
static void Finish(void* native) { Close(nullptr,native); Cleanup(*static_cast<Device*>(native)); }
static void Crc(uint8_t* data) {
    for(const auto range : {std::array<size_t,2>{0,0x34}, {0x36,8}, {0x40,0x44}}) {
        const auto crc=mosaico_board::ModuleCrc(data+range[0],range[1]);
        const auto end=range[0]+range[1]; data[end]=crc; data[end+1]=crc>>8;
    }
}
static Device& Native(uint16_t port) {
    for(auto& device:g_devices) if(device.live && device.port==port)return device;
    assert(false); std::abort();
}
static void Restore(unsigned slot) {
    assert(ConfigureSelector(slot)); g_slots[slot].restoring=false;
}
int main(int argc, char** argv) {
    const bool old=argc>1 && std::strstr(argv[1],"v1.0");
    const bool retry=argc>1 && std::strstr(argv[1],"retry");
    if(retry) add_failure_address=0x51;
    assert(EspMosaicoInitializePeripherals(reinterpret_cast<void*>(1), old?0x100:0x102,true));
    assert(bus_creates==(old?0:1));
    if(retry) {
        assert(!g_slots[1].eeprom && g_slots[1].info.flags==24);
        const int before=add_calls[0x51]; Scan(1,1000); assert(add_calls[0x51]==before);
        Scan(1,2000); assert(add_calls[0x51]==before+1 && !g_slots[1].eeprom);
        add_failure_address=-1;
        Scan(1,3000); assert(add_calls[0x51]==before+1);
        Scan(1,4000); Scan(1,4250); Scan(1,4500);
        assert(g_slots[1].eeprom && g_slots[1].info.flags==0);
    }
    assert(g_port_count==(old?24u:23u));
    assert(!Port(33)->name[31] && Pin(16)==53 && Pin(38)==38 && !Port(49));
    assert(!Port(40) && !Port(54)); // audio and USB pins stay owned by the board
    void* left=nullptr; void* other=nullptr;
    assert(OpenPrepared(nullptr,1,16,2,1,0,&left)==PXA_STATUS_OK && levels[53]==1);
    assert(OpenPrepared(nullptr,2,17,1,0,0,&other)==PXA_STATUS_BUSY);
    assert(OpenPrepared(nullptr,1,17,1,0,0,&other)==PXA_STATUS_OK);
    const int before=probes; Scan(0,3000); assert(probes==before);
    uint8_t value=0; assert(Io(nullptr,left,PXA_IO_WRITE,&value,1)==1 && !levels[53]);
    Finish(left); Finish(other);
    assert(g_slots[0].restoring && resets[53]);
    assert(ConfigureSelector(0)); g_slots[0].restoring=false;
    auto& eeprom=i2c[0x50]; eeprom.present=true;
    std::memcpy(eeprom.data,"ESP",3); eeprom.data[3]=2; eeprom.data[4]=42;
    std::memcpy(eeprom.data+0x14,"Camera",6); Crc(eeprom.data);
    for(unsigned i=0;i<3;++i) Scan(0,3000+i*250);
    assert(g_slots[0].info.flags==3 && g_slots[0].info.board_id==42);
    const auto generation=g_slots[0].info.generation;
    eeprom.data[4]=43; Crc(eeprom.data); Scan(0,6000);
    assert(g_slots[0].info.board_id==43 && g_slots[0].info.generation>generation);
    eeprom.data[0x40]^=1; Scan(0,9000); assert(g_slots[0].info.flags==17);
    eeprom.data[0x40]^=1; Scan(0,11000); assert(g_slots[0].info.flags==3);
    probe_errors[0x50]=true; Scan(0,11250); assert(g_slots[0].info.flags==19);
    probe_errors[0x50]=false; eeprom.present=false;
    Scan(0,11500); Scan(0,11750); Scan(0,12000);
    assert(g_slots[0].info.flags==0 && !g_slots[0].info.name[0]);
    probe_errors[0x50]=true; Scan(0,12250); assert(g_slots[0].info.flags==16);
    probe_errors[0x50]=false;
    Scan(0,12500); Scan(0,12750); Scan(0,13000); assert(g_slots[0].info.flags==0);
    eeprom.present=true; Scan(0,13250); Scan(0,13500);
    probe_errors[0x50]=true; Scan(0,13750); probe_errors[0x50]=false;
    Scan(0,14000); Scan(0,14250); assert(!(g_slots[0].info.flags&2));
    Scan(0,14500); assert(g_slots[0].info.flags==3);
    // A failed GPIO open owns its pins until native rollback succeeds.
    fail_config_pin=fail_reset_pin=53;
    assert(OpenPrepared(nullptr,1,16,2,1,0,&left)==PXA_STATUS_IO_ERROR && !left);
    Cleanup(Native(16)); assert(Native(16).closing && g_slots[0].references==1);
    assert(OpenPrepared(nullptr,2,17,1,0,0,&other)==PXA_STATUS_BUSY);
    fail_config_pin=fail_reset_pin=-1; Cleanup(Native(16)); Restore(0);

    void* device=nullptr;
    assert(OpenPrepared(nullptr,1,3,3,100000,0x50,&device)==PXA_STATUS_DENIED);
    const auto main_status=OpenPrepared(nullptr,1,3,3,100000,0x69,&device);
    assert(main_status==(old?PXA_STATUS_DENIED:PXA_STATUS_OK));
    if(!old) Finish(device);
    assert(OpenPrepared(nullptr,1,3,3,100000,0x20,&device)==PXA_STATUS_OK);
    assert(OpenPrepared(nullptr,2,3,3,100000,0x20,&other)==PXA_STATUS_BUSY);
    const uint8_t reg=10; i2c[0x20].data[10]=42;
    assert(Start(nullptr,device,100,&reg,1,1)==PXA_STATUS_OK);
    size_t bytes=1; assert(Poll(nullptr,device,100,&value,&bytes)==PXA_STATUS_WOULD_BLOCK);
    Transfer(*static_cast<Device*>(device));
    assert(Poll(nullptr,device,100,&value,&bytes)==PXA_STATUS_OK && value==42 && bytes==1);
    assert(Start(nullptr,device,101,&reg,1,1)==PXA_STATUS_OK);
    Cancel(nullptr,device,100); Transfer(*static_cast<Device*>(device)); bytes=1;
    assert(Poll(nullptr,device,101,&value,&bytes)==PXA_STATUS_OK); // stale cancel cannot affect new job
    i2c[0x20].fail=true; value=0xa5; bytes=1;
    assert(Start(nullptr,device,102,&reg,1,1)==PXA_STATUS_OK); Transfer(*static_cast<Device*>(device));
    assert(Poll(nullptr,device,102,&value,&bytes)==PXA_STATUS_IO_ERROR && bytes==0 && value==0xa5);
    i2c[0x20].fail=false; bytes=1;
    assert(Start(nullptr,device,103,&reg,1,1)==PXA_STATUS_OK); Transfer(*static_cast<Device*>(device));
    assert(Poll(nullptr,device,103,&value,&bytes)==PXA_STATUS_OK && value==42);
    assert(Start(nullptr,device,104,&reg,1,1)==PXA_STATUS_OK); Cancel(nullptr,device,104);
    Transfer(*static_cast<Device*>(device)); bytes=1; value=0xa5;
    assert(Poll(nullptr,device,104,&value,&bytes)==PXA_STATUS_CANCELLED && bytes==0 && value==0xa5);
    Close(nullptr,device); remove_fails=true; Cleanup(*static_cast<Device*>(device));
    assert(static_cast<Device*>(device)->live);
    assert(OpenPrepared(nullptr,2,3,3,100000,0x20,&other)==PXA_STATUS_BUSY);
    remove_fails=false; Cleanup(*static_cast<Device*>(device));
    assert(OpenPrepared(nullptr,2,3,3,10000,0x20,&device)==PXA_STATUS_OK);
    uint8_t slow_tx[128]{}, slow_rx[128]{};
    assert(Start(nullptr,device,102,slow_tx,128,128)==PXA_STATUS_OK);
    Transfer(*static_cast<Device*>(device)); bytes=128;
    assert(Poll(nullptr,device,102,slow_rx,&bytes)==PXA_STATUS_OK && last_timeout>=254 && last_timeout<=500);
    Finish(device);
    assert(OpenPrepared(nullptr,2,4,4,115200,0,&device)==PXA_STATUS_OK && uart_live);
    assert(Io(nullptr,device,PXA_IO_READ,&value,1)==PXA_STATUS_WOULD_BLOCK);
    assert(Io(nullptr,device,PXA_IO_WRITE,&value,1)==1);
    fail_uart_delete=true; Close(nullptr,device); Cleanup(Native(4)); assert(uart_live);
    fail_uart_delete=false; fail_reset_pin=58; Cleanup(Native(4));
    assert(!uart_live && Native(4).closing);
    const auto deleted=uart_deletes; Cleanup(Native(4)); assert(uart_deletes==deleted);
    assert(OpenPrepared(nullptr,1,32,1,0,0,&other)==PXA_STATUS_BUSY);
    fail_reset_pin=-1; Cleanup(Native(4)); assert(uart_deletes==deleted); Restore(1);
    fail_uart_install=true; fail_reset_pin=59;
    assert(OpenPrepared(nullptr,2,4,4,115200,0,&device)==PXA_STATUS_IO_ERROR && !device && !uart_live);
    Cleanup(Native(4)); assert(Native(4).closing && uart_deletes==deleted);
    fail_uart_install=false; fail_reset_pin=-1; Cleanup(Native(4)); Restore(1);
    assert(OpenPrepared(nullptr,2,4,4,115200,0,&device)==PXA_STATUS_OK); Finish(device); Restore(1);
    if(old) {
        assert(OpenPrepared(nullptr,1,48,2,0,0,&device)==PXA_STATUS_OK && levels[3]==1);
        value=1; assert(Io(nullptr,device,PXA_IO_WRITE,&value,1)==1 && levels[3]==0);
        fail_level_pin=3; Close(nullptr,device); Cleanup(Native(48)); assert(Native(48).closing);
        assert(OpenPrepared(nullptr,2,48,2,0,0,&other)==PXA_STATUS_BUSY);
        fail_level_pin=-1; Cleanup(Native(48)); assert(levels[3]==1);
    }
    assert(OpenPrepared(nullptr,1,16,2,1,0,&left)==PXA_STATUS_OK && levels[53]==1);
    assert(OpenPrepared(nullptr,1,3,3,100000,0x20,&device)==PXA_STATUS_OK);
    g_shutdown=true;
    value=0;
    assert(Io(nullptr,left,PXA_IO_WRITE,&value,1)==PXA_STATUS_UNAVAILABLE && levels[53]==1);
    assert(Start(nullptr,device,999,&reg,1,1)==PXA_STATUS_UNAVAILABLE && !Native(3).pending);
    assert(OpenPrepared(nullptr,1,16,1,0,0,&other)==PXA_STATUS_UNAVAILABLE);
    // Once the worker has drained all I/O, callbacks can no longer notify its TCB.
    Finish(left); Finish(device); g_notifications.Stop(); worker_deleted=true;
    const auto notified=notify_calls;
    Cancel(nullptr,device,999); Close(nullptr,device);
    assert(notify_calls==notified);

    std::free(allocation);
    puts("Mosaico ports: revisions, ownership, pin reservations, async I2C, EEPROM recovery, failed-open rollback, staged UART cleanup and I/O errors passed");
}
