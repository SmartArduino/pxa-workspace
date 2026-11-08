#pragma once

class WatcherBattery;

// Board startup only. The battery monitor must outlive the Sensor service.
void WatcherInitializeSensors(WatcherBattery* battery);
