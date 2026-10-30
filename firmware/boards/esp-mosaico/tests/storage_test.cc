#include "../src/esp_mosaico_storage.cc"
#include <cstdio>
#include <cstdlib>

static unsigned registrations=0,media_selected=0;
extern "C" pxa_status_t pxa_esp_storage_register_volume(const pxa_esp_storage_volume_config_t* config,uint64_t* epoch) {
    ++registrations;
    assert(!std::strcmp(config->mount_path,"/pxa/nand"));
    assert(!(config->capabilities&PXA_VOLUME_REMOVABLE));
    assert(config->capabilities&PXA_VOLUME_INSTALLABLE);
    assert(config->allocation_unit==65536 && config->link_free);
    assert(config->device_identity_bytes==32);
    auto* identity=static_cast<const uint8_t*>(config->device_identity);
    assert(identity[0]==1 && identity[5]==6 && !std::memcmp(identity+6,"mosaico-spi3-nand",17));
    if(test_fail_stage==5)assert(!config->space_known);
    else assert(config->total_bytes==100*1024*1024 && config->free_bytes==80*1024*1024);
    uint64_t total=0,free=0;uint8_t known=0;
    auto space=config->query_space(nullptr,config->mount_path,&total,&free,&known);
    assert((test_fail_stage==5 && space==PXA_STATUS_IO_ERROR && !known) ||
        (space==PXA_STATUS_OK && known==3 && total==100*1024*1024));
    *epoch=1;return (test_fail_stage==7 || test_fail_stage==8)?PXA_STATUS_IO_ERROR:PXA_STATUS_OK;
}
extern "C" pxa_status_t pxa_esp_storage_select_app_media(const char* path) {
    assert(!std::strcmp(path,"/pxa/nand"));++media_selected;return PXA_STATUS_OK;
}
int main(int argc,char** argv) {
#ifdef CONFIG_PXA_MOSAICO_NAND_FORMAT_IF_MOUNT_FAILED
    if(argc>1 && (argv[1][0]<'0' || argv[1][0]>'9')) {
        const char* scenario=argv[1];
        const bool existing=!std::strcmp(scenario,"existing");
        const bool missing=!std::strcmp(scenario,"missing");
        test_probe_result=existing?FR_OK:FR_NO_FILESYSTEM;
        if(!std::strcmp(scenario,"probe-io"))test_probe_result=FR_DISK_ERR;
        if(!std::strcmp(scenario,"probe-internal"))test_probe_result=FR_INT_ERR;
        test_probe_drive_fail=!std::strcmp(scenario,"probe-drive");
        test_probe_register_fail=!std::strcmp(scenario,"probe-register");
        test_probe_unmount_fail=!std::strcmp(scenario,"probe-unmount");
        assert(EspMosaicoInitializeStorage()==(existing || missing));
        if(existing || missing) {
            assert(test_format_calls==(missing?1:0) && test_mount_calls==1);
            assert(registrations==1 && media_selected==1 && !test_probe_registered && !test_probe_fs);
        } else if(test_probe_unmount_fail) {
            assert(g_wl && g_device && g_bus_owned && !g_ready && test_probe_fs);
            assert(!test_bdl_released && !test_mount_calls && !test_format_calls);
            assert(!EspMosaicoInitializeStorage());
        } else {
            assert(!g_wl && !g_device && !g_bus_owned && !test_probe_registered && !test_probe_fs);
            assert(!test_mount_calls && !test_format_calls && !registrations);
        }
        puts("NAND auto-initialization: only FR_NO_FILESYSTEM permits a format; existing data and probe failures are preserved");
        return 0;
    }
#endif
    if(argc>1)test_fail_stage=std::atoi(argv[1]);
    const int stage=test_fail_stage;
    if(stage==15)test_nand_config=0x11; // Warm restart must not rewrite a set QE bit.
    if(stage==20 || stage==21) {
        test_fail_stage=0;
        test_bootstrap_remove_fail=stage==20;test_bootstrap_free_fail=stage==21;
        assert(!EspMosaicoInitializeStorage());
        assert(!g_wl && !g_ready && g_bus_owned && test_spi_live);
        assert(bool(g_device)==(stage==20));
        assert(!EspMosaicoInitializeStorage() && test_spi_initializes==1);
        test_bootstrap_remove_fail=test_bootstrap_free_fail=false;
        assert(EspMosaicoInitializeStorage() && test_spi_initializes==3);
        assert(registrations==1 && media_selected==1 && test_quad_writes==1);
        puts("NAND: bootstrap cleanup failure retains ownership and safely retries");
        return 0;
    }
    if(stage==9 || stage==10) {
        test_fail_stage=4; // Fail mount, then inject an error during native rollback.
        test_spi_remove_fail=stage==9; test_spi_free_fail=stage==10;
        assert(!EspMosaicoInitializeStorage());
        assert(!g_wl && !g_ready && g_bus_owned && test_spi_live);
        assert(bool(g_device)==(stage==9));
        assert(!EspMosaicoInitializeStorage() && test_spi_initializes==2);
        test_spi_remove_fail=test_spi_free_fail=false; test_fail_stage=0;
        assert(EspMosaicoInitializeStorage() && test_spi_initializes==4 && registrations==1 && media_selected==1);
        puts("NAND: failed SPI rollback retains ownership and a retry safely remounts storage");
        return 0;
    }
    const bool expected=stage==0 || stage==5 || stage==15;
    assert(EspMosaicoInitializeStorage()==expected);
    if(expected) {
        assert(registrations==1 && media_selected==1 && test_bus_free==1);
        assert(test_quad_writes==(stage==15?0:1) && test_nand_config==0x11);
        assert(EspMosaicoInitializeStorage());assert(registrations==1);
    } else if(test_fail_stage==8) {
        assert(!media_selected && g_wl && g_device && !g_ready);
        assert(test_bus_free==1 && !test_bdl_released && test_unmounted==1);
        assert(!EspMosaicoInitializeStorage());assert(registrations==1);
    } else {
        assert(!media_selected && !g_wl && !g_device);
        const bool reached_quad=stage==3 || stage==4 || stage==6 || stage==7 || stage==19;
        assert(test_bus_free==((stage==1 || stage==11)?0:(reached_quad?2:1)));
        assert(test_spi_removed==((stage==1 || stage==2 || stage==11)?0:
            ((stage==3 || stage==4 || stage==6 || stage==7)?2:1)));
        assert(test_bdl_released==((stage==4 || stage==6 || stage==7)?1:0));
        assert(test_unmounted==((stage==6 || stage==7)?1:0));
    }
    puts("NAND: HOLD/WP release, single-line ID/QE bootstrap, QIO handoff, format policy and failure cleanup passed");
}
