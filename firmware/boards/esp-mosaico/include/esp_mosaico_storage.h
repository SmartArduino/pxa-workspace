#pragma once

// Call once during board initialization, after power and hardware detection.
// An absent/unmountable NAND is optional and does not prevent system startup.
bool EspMosaicoInitializeStorage();
