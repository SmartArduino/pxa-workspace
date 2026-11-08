[中文](README.md) | English

# p4_wireless

`p4_wireless` provides Wi-Fi and BLE for the ESPP4-A module. Applications use the ESP-IDF `esp_wifi`, `esp_netif`, event loop and NimBLE APIs, and the component talks to the wireless chip inside the module.

The component ships as a prebuilt static library plus a few ESP-IDF adaptation sources that are built with your project. The wireless firmware is loaded by the driver, and the application starts the driver with `esp_wifi_init()`.

## Requirements

| Item | Requirement |
| --- | --- |
| Hardware | ESPP4-A module |
| Target | `esp32p4` |
| ESP-IDF | `5.5 <= version < 7.0` |
| Verified versions | `5.5.3`, `6.0.1`, `6.2.0` |
| Wi-Fi mode | STA or SoftAP, `WIFI_MODE_APSTA` is not supported |
| BLE host | ESP-IDF NimBLE, Bluedroid is not supported |

Other ESP-IDF versions in the supported range produce a CMake warning and need your own verification. The component is bound to the ESPP4-A module; on other modules initialization may return `ESP_FAIL`.

## Run an Example

Start with the [wifi_cmd example](examples/wifi_cmd/) to check your toolchain and the module. With the ESP-IDF environment loaded:

```sh
cd examples/wifi_cmd
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Use your actual serial port, and press `Ctrl+]` to exit the monitor. At the `wifi>` prompt, type:

```text
scan
sta "your-ssid" "your-password"
```

When the log shows `got ip:`, the STA is connected and has an IP address. See the example README for more commands (SoftAP, ping, iperf).

| Example | Description |
| --- | --- |
| [wifi_cmd](examples/wifi_cmd/) | Serial console to scan, connect, start/stop SoftAP, show info and run iperf |
| [blufi](examples/blufi/) | Builds the official ESP-IDF BluFi example to provision Wi-Fi over BLE |

The first `wifi_cmd` build downloads the `espressif/iperf` dependency. Use iperf 2.x on the peer for throughput measurements.

## Add to Your Project

### 1. Copy the component

Put the whole `p4_wireless` directory under your project's `components/`, keeping its layout:

```text
your_project/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.c
└── components/
    └── p4_wireless/
        ├── CMakeLists.txt
        ├── Kconfig
        ├── include/
        ├── include_compat/
        ├── lib/libp4_wireless.a
        └── src/
```

Add the dependencies to the component that calls the wireless APIs, for example:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES p4_wireless esp_wifi esp_netif esp_event nvs_flash
)
```

Keep the package's CMake, Kconfig, compatibility headers and adaptation sources; the component picks what the current ESP-IDF needs.

### 2. Required configuration

Merge the following into your project's `sdkconfig.defaults`:

```ini
CONFIG_FREERTOS_HZ=1000
CONFIG_LWIP_IPV6=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_250M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

`FREERTOS_HZ` and `LWIP_IPV6` must match the prebuilt library, otherwise CMake stops the build. ESP32-P4 PSRAM uses Hex mode, and PSRAM must be enabled because the SDIO clock depends on the MSPI clock domain.

ESP-IDF 6 also needs newlib:

```ini
CONFIG_LIBC_NEWLIB=y
```

On ESP-IDF 6.1 and later, which has a built-in Wi-Fi Remote interface, keep the slave target as `ESP32C6` (`CONFIG_SLAVE_IDF_TARGET_ESP32C6=y`). The IDF compatibility interface requires it.

Wi-Fi buffer counts and similar parameters are fixed by the prebuilt library. Keep `CONFIG_ESP_WIFI_REMOTE_MAX_CONN_NUM` at `15` and do not override these parameters.

If an `sdkconfig` already exists, editing `sdkconfig.defaults` does not change it; update the values with `idf.py menuconfig` as well.

### 3. App partition

The default 1 MB app partition may be too small with the wireless library. You can start with the [wifi_cmd partition table](examples/wifi_cmd/partitions.csv) (3 MB `factory` app partition): copy it to your project root as `partitions.csv` and add:

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x9000
```

NVS in this table starts at `0xa000`, matching the partition table offset above. Set the flash size for your hardware; the examples use 16 MB. Projects with an OTA layout should keep their own layout and leave enough space in each app partition.

### 4. Initialize Wi-Fi

STA setup order (full code in [wifi_cmd main.c](examples/wifi_cmd/main/main.c), `app_main()` and `cmd_sta()`):

1. Call `nvs_flash_init()`, `esp_netif_init()` and `esp_event_loop_create_default()`.
2. Call `esp_netif_create_default_wifi_sta()` to create the network interface.
3. Create the config with `WIFI_INIT_CONFIG_DEFAULT()`, call `esp_wifi_init()` and check the return value.
4. Register handlers for `WIFI_EVENT` and `IP_EVENT_STA_GOT_IP`.
5. Call `esp_wifi_set_mode(WIFI_MODE_STA)`, `esp_wifi_set_config()` and `esp_wifi_start()`.
6. On `WIFI_EVENT_STA_START`, call `esp_wifi_connect()`, then wait for `IP_EVENT_STA_GOT_IP` before using the network.

A successful `esp_wifi_connect()` only means the request was accepted. The result and IP address are reported by events, and reconnecting is up to the application.

For SoftAP, create `esp_netif_create_default_wifi_ap()`, select `WIFI_MODE_AP`, set the SSID, password and channel in `wifi_config_t.ap`, then call `esp_wifi_start()`. To switch between STA and SoftAP after starting, call `esp_wifi_stop()` first.

## BLE / BluFi

Add to the configuration above:

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_CONTROLLER_DISABLED=y
# CONFIG_BT_NIMBLE_TRANSPORT_UART is not set
```

In `menuconfig`, select NimBLE, set the controller to `Disabled` and disable NimBLE's UART transport. The component then builds its HCI adaptation source and reaches the Bluetooth controller in the module over SDIO.

`esp_wifi_init()` must succeed before `nimble_port_init()`, because the Bluetooth firmware is loaded with the wireless driver. This also applies when you only use BLE.

BluFi also needs `CONFIG_BT_NIMBLE_BLUFI_ENABLE=y`. The [blufi example](examples/blufi/) already contains the configuration and build changes the official example needs.

## API and Behavior Differences

Wi-Fi applications include `esp_wifi.h` and use the standard `esp_wifi_*` APIs. Supported features: driver init/start/stop, STA connect/disconnect, scan results, SoftAP start/stop and station list, MAC address get/set, current channel and power save mode.

When porting an existing ESP-IDF application, check these differences:

| Feature | Current behavior |
| --- | --- |
| STA + SoftAP concurrency | `WIFI_MODE_APSTA` returns `ESP_ERR_NOT_SUPPORTED` |
| Wi-Fi config persistence | Config is kept in RAM only; `WIFI_STORAGE_FLASH` is treated as RAM, so the application must set it again after reboot |
| STA / AP MAC | Both modes share one MAC, which can only be changed before `esp_wifi_start()` |
| `esp_wifi_set_channel()` | Returns `ESP_ERR_NOT_SUPPORTED`; the STA channel follows the AP, the SoftAP channel is set in `wifi_config_t.ap.channel` |
| Country code, max TX power | Return `ESP_ERR_NOT_SUPPORTED` |
| STA / AP station AID, deauth by AID | Return `ESP_ERR_NOT_SUPPORTED` |
| Promiscuous mode, CSI, ESP-NOW | Not supported |

A declaration in a header does not mean the feature is implemented; always check API return values.

### Extension API

Include [p4_wireless.h](include/p4_wireless.h) to query the wireless chip:

| API | Purpose and limits |
| --- | --- |
| `p4_wireless_get_fw_version(buf, len)` | Wireless firmware version, call after the driver is initialized |
| `p4_wireless_get_data_rate(&tx, &rx)` | Negotiated PHY rate in bps, call after the STA is associated; `rx` is always `0` for now |
| `p4_wireless_get_temperature(&temp)` | Temperature reading is not reliable yet, do not use it for thermal protection or control |
| `p4_wireless_hci_open()`, `p4_wireless_hci_send()` | Used by the HCI adaptation layer, NimBLE applications do not call them |

The PHY rate shows the radio link state; measure TCP / UDP throughput separately.

## Hardware Resources

The wireless chip in the module uses SDMMC Slot 1 in 4-bit SDIO mode, with fixed connections:

| Resource | Use |
| --- | --- |
| GPIO18 / GPIO19 | SDIO CLK / CMD |
| GPIO14 / GPIO15 / GPIO16 / GPIO17 | SDIO D0 / D1 / D2 / D3 |
| GPIO54 | Wireless chip reset |

Do not reuse these GPIOs or SDMMC Slot 1 in your application. Use Slot 0 for an SD card, wired as on the development board. eFuse `BLOCK_KEY5` is used by module authorization, do not write other keys to it.

## FAQ

| Symptom | What to check |
| --- | --- |
| CMake reports `CONFIG_FREERTOS_HZ=1000`, `CONFIG_LWIP_IPV6=y` or `CONFIG_LIBC_NEWLIB=y` | Check the actual value in `sdkconfig`; editing defaults does not change an existing config |
| CMake reports a wrong slave target | Set the Wi-Fi Remote slave target back to `ESP32C6` |
| `warning: unknown kconfig symbol` | The option does not exist in this ESP-IDF or target; check against this page. ESP32-P4 PSRAM mode is `CONFIG_SPIRAM_MODE_HEX` |
| `app partition is too small` | Check the binary size and enlarge the app partition |
| `esp_wifi_init()` returns `ESP_FAIL` | Keep the full boot log; check that it is an ESPP4-A module, that power is stable, and that the SDIO / reset pins are not used elsewhere |
| NimBLE prints `HCI open failed` | Make sure `esp_wifi_init()` returned successfully before calling `nimble_port_init()` |
| An API returns `ESP_ERR_NOT_SUPPORTED` | Check the feature list above; not every on-chip Wi-Fi feature is available |
| SSID / password lost after reboot | Save them in NVS in your application and call `esp_wifi_set_config()` again at startup |

When reporting an issue, include the full ESP-IDF version, the example or minimal code that reproduces it, the related configuration, API return values and the full boot log.
