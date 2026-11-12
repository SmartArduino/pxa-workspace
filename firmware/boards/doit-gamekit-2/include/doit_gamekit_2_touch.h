#pragma once
#include "doit_gamekit_2_touch_report.h"
#include <driver/i2c_master.h>

class DoitGamekit2Touch {
public:
    bool Initialize();
    bool Read(doit_gamekit_2::LandscapeRotation rotation, doit_gamekit_2::TouchReport& report);
private:
    void Cleanup();
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
};
