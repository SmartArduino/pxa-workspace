#include "../src/esp_mosaico_sensors.cc"
#include <cstdio>

static void Put16(TestI2c& d, uint8_t reg, int16_t value) {
    d.reg[reg]=uint16_t(value)&255;d.reg[reg+1]=uint16_t(value)>>8;
}
int main(int argc,char** argv) {
    const bool missing=argc>1 && !std::strcmp(argv[1],"missing");
    const bool partial=argc>1 && !std::strcmp(argv[1],"partial");
    if (!missing) {
        test_i2c[0x69].present=true;
        test_i2c[0x69].reg[0]=0x24;
        test_i2c[0x69].reg[0x21]=1;
        for(unsigned address:{0x11u,0x12u}) {
            test_i2c[address].present=true;
            test_i2c[address].reg[0x40]=0x32;
        }
    }
    if(partial) {
        test_i2c[0x69].reg[0]=0; // responds, but is not a BMI270
        test_i2c[0x11].reg[0x40]=0; // incorrect magnetometer identity
    }
    const bool with_battery=argc>1 && !std::strcmp(argv[1],"battery");
    EspMosaicoBattery battery;
    if(with_battery) {
        test_i2c[0x55].present=true;
        battery.Poll(0,[](uint8_t reg,uint8_t* output,size_t size) {
            const uint16_t words[]={2981,3900,8,uint16_t(int16_t(-80))};
            if(reg==0x06) { assert(size==8); for(size_t i=0;i<4;++i) { output[2*i]=words[i]; output[2*i+1]=words[i]>>8; } }
            else { assert(reg==0x2c && size==2); output[0]=67; output[1]=0; }
            return true;
        });
    }
    EspMosaicoInitializeSensors(nullptr, with_battery?&battery:nullptr);
    pxa_sensor_config_t config{};
    config.struct_size=sizeof(config);config.max_subscriptions=8;
    pxa_esp_sensor_configure(&config);
    if(missing) {
        assert(!config.descriptor_count);assert(!config.read);
        puts("Absent sensor chips expose no capabilities");return 0;
    }
    if(partial) {
        assert(config.descriptor_count==1 && config.descriptors[0].id==5);
        assert(!g_imu_present && !g_mag_present[0] && g_mag_present[1]);
        assert(test_i2c_removed==2);
        puts("Failed chips are isolated; the other magnetometer remains available");return 0;
    }
    assert(config.descriptor_count==(with_battery?8u:5u));
    const char* motion_semantics[]={"motion.acceleration","motion.angular-velocity"};
    for(unsigned i=0;i<2;++i) {
        assert(config.descriptors[i].semantic.size==std::strlen(motion_semantics[i]));
        assert(!std::memcmp(config.descriptors[i].semantic.data,motion_semantics[i],
            config.descriptors[i].semantic.size));
    }
    if(with_battery) {
        const int32_t expected[]={3900000,-80000,24950};
        for(unsigned i=0;i<3;++i) {
            void* subscription=nullptr; int32_t sample[3];
            assert(config.subscribe(nullptr,6+i,2000,&subscription)==PXA_STATUS_OK);
            assert(config.read(nullptr,subscription,6+i,sample)==PXA_STATUS_OK && sample[0]==expected[i]);
            config.unsubscribe(nullptr,subscription,6+i);
        }
        assert(config.descriptors[5].unit==6 && config.descriptors[6].unit==7);
    }
    assert(config.max_subscriptions==8);
    assert(config.descriptors[0].unit==2 && config.descriptors[1].unit==3);
    assert(config.descriptors[3].unit==5 && config.descriptors[4].id==5);
    void *acc=nullptr,*gyro=nullptr,*temp=nullptr;
    assert(config.subscribe(nullptr,1,10,&acc)==PXA_STATUS_OK);
    assert(config.subscribe(nullptr,2,10,&gyro)==PXA_STATUS_OK);
    assert(config.subscribe(nullptr,3,100,&temp)==PXA_STATUS_OK);
    assert(g_imu_bus.users==3 && acc==gyro && gyro==temp);
    int32_t values[3];
    assert(config.read(nullptr,acc,1,values)==PXA_STATUS_UNAVAILABLE);
    test_now_us+=100000;
    Put16(test_i2c[0x69],0x0c,8192); // +1g at +/-4g
    Put16(test_i2c[0x69],0x0e,-8192);
    Put16(test_i2c[0x69],0x10,0);
    Put16(test_i2c[0x69],0x12,16384); // +1000 degrees/s
    Put16(test_i2c[0x69],0x14,-16384);
    Put16(test_i2c[0x69],0x16,0);
    Put16(test_i2c[0x69],0x22,512);
    assert(config.read(nullptr,acc,1,values)==PXA_STATUS_OK);
    assert(values[0]==-9806650 && values[1]==-9806650 && values[2]==0);
    assert(config.read(nullptr,gyro,2,values)==PXA_STATUS_OK);
    assert(values[0]==-17453292 && values[1]==-17453292 && values[2]==0);
    // Read all six faces through the real Bosch driver and provider boundary.
    // In particular, screen-up/down Z must not change sign to fix the XY map.
    const int expected_axis[]={1,0,2};
    const int expected_sign[]={-1,1,1};
    for(unsigned axis=0;axis<3;++axis) for(int sign:{-1,1}) {
        for(unsigned i=0;i<3;++i) {
            Put16(test_i2c[0x69],0x0c+2*i,i==axis?sign*8192:0);
            Put16(test_i2c[0x69],0x12+2*i,i==axis?sign*16384:0);
        }
        for(unsigned id:{1u,2u}) {
            assert(config.read(nullptr,id==1?acc:gyro,id,values)==PXA_STATUS_OK);
            for(unsigned i=0;i<3;++i) {
                const int32_t magnitude=id==1?9806650:17453292;
                assert(values[i]==(i==unsigned(expected_axis[axis])?
                    sign*expected_sign[axis]*magnitude:0));
            }
        }
    }
    // A proper rotation preserves handedness: T(X) cross T(Y) == T(Z).
    const auto x_axis=mosaico_board::ImuDeviceFrame(1,0,0);
    const auto y_axis=mosaico_board::ImuDeviceFrame(0,1,0);
    const auto z_axis=mosaico_board::ImuDeviceFrame(0,0,1);
    assert(x_axis[1]*y_axis[2]-x_axis[2]*y_axis[1]==z_axis[0]);
    assert(x_axis[2]*y_axis[0]-x_axis[0]*y_axis[2]==z_axis[1]);
    assert(x_axis[0]*y_axis[1]-x_axis[1]*y_axis[0]==z_axis[2]);
    const auto minimum=mosaico_board::ImuDeviceFrame(
        mosaico_board::AccelerationMicro(INT16_MIN),
        mosaico_board::AccelerationMicro(INT16_MIN),
        mosaico_board::AccelerationMicro(INT16_MIN));
    assert(minimum[0]==-39226600 && minimum[1]==39226600 && minimum[2]==-39226600);
    assert(config.read(nullptr,temp,3,values)==PXA_STATUS_OK && values[0]==24000);
    test_i2c[0x69].fail=true;
    assert(config.read(nullptr,gyro,2,values)==PXA_STATUS_IO_ERROR);
    test_i2c[0x69].fail=false;
    config.unsubscribe(nullptr,acc,1);config.unsubscribe(nullptr,gyro,2);
    assert(g_imu_bus.users==1 && (test_i2c[0x69].reg[0x7d]&6)==6);
    config.unsubscribe(nullptr,temp,3);
    assert(g_imu_bus.users==0 && (test_i2c[0x69].reg[0x7d]&6)==0);
    assert(config.read(nullptr,acc,1,values)==PXA_STATUS_NOT_FOUND);
    void* mag=nullptr;
    assert(config.subscribe(nullptr,4,100,&mag)==PXA_STATUS_OK);
    test_now_us+=150000;
    // Zero trim is invalid and must not fabricate a valid magnetic sample.
    assert(config.read(nullptr,mag,4,values)==PXA_STATUS_IO_ERROR);
    // Known trim/raw fixture exercises the pinned Bosch compensation itself:
    // X/Y are +/-25 microtesla, Z is 11 microtesla after integer truncation.
    auto& trim=g_mag[0].trim_data;
    trim.dig_xyz1=1000;trim.dig_z1=32768;trim.dig_z2=16384;
    Put16(test_i2c[0x11],0x42,80*8);
    Put16(test_i2c[0x11],0x44,-80*8);
    Put16(test_i2c[0x11],0x46,100*2);
    Put16(test_i2c[0x11],0x48,1000*4);
    assert(config.read(nullptr,mag,4,values)==PXA_STATUS_OK);
    assert(values[0]==25000 && values[1]==-25000 && values[2]==11000);
    config.unsubscribe(nullptr,mag,4);
    assert(g_mag_bus[0].users==0 && (test_i2c[0x11].reg[0x4c]&6)==6);
    assert(mosaico_board::MagneticNano(16)==16000);
    assert(mosaico_board::MagneticNano(-16)==-16000);
    if(with_battery) {
        void* all[8]{};
        for(unsigned id=1;id<=8;++id) {
            const unsigned period=id>=6?2000:(id<=2?10:100);
            assert(config.subscribe(nullptr,id,period,&all[id-1])==PXA_STATUS_OK);
        }
        assert(g_imu_bus.users==3 && g_mag_bus[0].users==1 && g_mag_bus[1].users==1);
        for(unsigned id=1;id<=8;++id) config.unsubscribe(nullptr,all[id-1],id);
    }
    pxa_esp_sensor_set_provider(nullptr);pxa_sensor_config_t cleared{};
    pxa_esp_sensor_configure(&cleared);assert(!cleared.descriptor_count);
    puts("Bosch drivers + sensor provider: initialization, five channels, units, shared power, I2C failures and overflow passed");
}
