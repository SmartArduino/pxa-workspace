#pragma once
#include "doit_gamekit_1_touch_report.h"
#include <driver/i2c_master.h>

class DoitGamekit1Touch {
public:
    bool Initialize();
    bool Read(doit_gamekit_1::LandscapeRotation rotation, doit_gamekit_1::TouchReport& report);
private:
    void Cleanup();
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
};
