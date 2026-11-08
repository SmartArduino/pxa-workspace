中文 | [English](README_EN.md)

# p4_wireless

`p4_wireless` 为 ESPP4-A 模组提供 Wi-Fi 和 BLE 接入能力。应用通过 ESP-IDF 的 `esp_wifi`、`esp_netif`、事件系统和 NimBLE API 开发，组件负责与模组内的无线芯片通信。

组件以预编译静态库发布，附带随工程编译的 ESP-IDF 适配源码。无线固件由驱动初始化时加载，应用使用 `esp_wifi_init()` 启动底层驱动。

## 适用环境

| 项目 | 要求 |
| --- | --- |
| 硬件 | 配套 ESPP4-A 模组 |
| 编译目标 | `esp32p4` |
| ESP-IDF | `5.5 <= 版本 < 7.0` |
| 已验证版本 | `5.5.3`、`6.0.1`、`6.2.0` |
| Wi-Fi 模式 | STA 或 SoftAP，不支持 `WIFI_MODE_APSTA` |
| BLE host | ESP-IDF 自带的 NimBLE，不支持 Bluedroid |

版本范围内的其他 ESP-IDF 版本会收到 CMake 警告，需要自行验证。组件与配套模组绑定，非配套模组初始化可能返回 `ESP_FAIL`。

## 快速运行示例

建议先运行包内的 [wifi_cmd 示例](examples/wifi_cmd/)，确认编译环境和模组工作正常。加载 ESP-IDF 环境后执行：

```sh
cd examples/wifi_cmd
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

串口名按实际设备修改，按 `Ctrl+]` 退出监视器。启动后在 `wifi>` 提示符下输入：

```text
scan
sta "你的WiFi名称" "密码"
```

串口输出 `got ip:` 表示 STA 已连接并取得 IP 地址。更多命令（SoftAP、ping、iperf）见例程 README。

| 示例 | 用途 |
| --- | --- |
| [wifi_cmd](examples/wifi_cmd/) | 通过串口控制台扫描、连接、启停 SoftAP、查看信息和运行 iperf |
| [blufi](examples/blufi/) | 使用 ESP-IDF 官方 BluFi 示例，通过 BLE 配置 Wi-Fi |

`wifi_cmd` 首次构建需要下载 `espressif/iperf` 依赖。吞吐测量的对端使用 iperf 2.x。

## 接入自己的工程

### 1. 复制组件

将整个 `p4_wireless` 目录放入工程的 `components/` 下，保留目录结构：

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

在调用无线接口的组件 `CMakeLists.txt` 中补充依赖。例如：

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES p4_wireless esp_wifi esp_netif esp_event nvs_flash
)
```

保留发布包的 CMake、Kconfig、兼容头文件和适配源码，由组件自动选择当前 ESP-IDF 所需的实现。

### 2. 必须满足的配置

将以下配置合并到工程的 `sdkconfig.defaults`：

```ini
CONFIG_FREERTOS_HZ=1000
CONFIG_LWIP_IPV6=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_250M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

`FREERTOS_HZ` 和 `LWIP_IPV6` 必须与预编译库一致，否则 CMake 会终止构建。ESP32-P4 的 PSRAM 使用 Hex 模式；SDIO 时钟依赖的 MSPI 时钟域需要启用 PSRAM。

ESP-IDF 6 还需要选择 newlib：

```ini
CONFIG_LIBC_NEWLIB=y
```

ESP-IDF 6.1 及以上使用 IDF 内置的 Wi-Fi Remote 接口时，将 slave 目标保留为 `ESP32C6`（`CONFIG_SLAVE_IDF_TARGET_ESP32C6=y`）。这是 IDF 兼容接口所需的配置。

Wi-Fi 的缓冲区数量等参数已按预编译库固定，`CONFIG_ESP_WIFI_REMOTE_MAX_CONN_NUM` 应保持 `15`；不要自行覆盖这些参数。

已有 `sdkconfig` 时，修改 `sdkconfig.defaults` 不会覆盖已保存的配置，还需通过 `idf.py menuconfig` 同步修改。

### 3. 配置应用分区

接入无线库后，默认的 1 MB 应用分区可能不足。首次接入可直接使用 [wifi_cmd 的分区表](examples/wifi_cmd/partitions.csv)（3 MB `factory` 应用分区），将其复制为工程根目录的 `partitions.csv`，并加入：

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x9000
```

该分区表的 NVS 从 `0xa000` 开始，需与上述分区表偏移配套使用。Flash 容量按实际硬件设置，配套示例使用 16 MB。已有 OTA 布局的工程应保留自己的布局，并为每个应用分区预留足够空间。

### 4. 初始化 Wi-Fi

STA 接入顺序如下，完整代码见 [wifi_cmd 的 main.c](examples/wifi_cmd/main/main.c)（`app_main()` 和 `cmd_sta()`）：

1. 调用 `nvs_flash_init()`、`esp_netif_init()` 和 `esp_event_loop_create_default()`。
2. 调用 `esp_netif_create_default_wifi_sta()` 创建网络接口。
3. 用 `WIFI_INIT_CONFIG_DEFAULT()` 创建配置，调用 `esp_wifi_init()`，检查返回值。
4. 注册 `WIFI_EVENT` 和 `IP_EVENT_STA_GOT_IP` 处理函数。
5. 调用 `esp_wifi_set_mode(WIFI_MODE_STA)`、`esp_wifi_set_config()` 和 `esp_wifi_start()`。
6. 收到 `WIFI_EVENT_STA_START` 后调用 `esp_wifi_connect()`，等待 `IP_EVENT_STA_GOT_IP` 再发起网络业务。

`esp_wifi_connect()` 返回成功表示连接请求已提交。连接结果和 IP 地址通过事件通知；断线重连由应用处理。

使用 SoftAP 时，创建 `esp_netif_create_default_wifi_ap()`，选择 `WIFI_MODE_AP`，通过 `wifi_config_t.ap` 设置 SSID、密码和信道，然后调用 `esp_wifi_start()`。已启动后切换 STA / SoftAP，须先调用 `esp_wifi_stop()`。

## 使用 BLE / BluFi

在前述工程配置上增加：

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_CONTROLLER_DISABLED=y
# CONFIG_BT_NIMBLE_TRANSPORT_UART is not set
```

在 `menuconfig` 中选择 NimBLE，controller 选择 `Disabled`，关闭 NimBLE 的 UART transport。组件会自动编译 HCI 适配源码，通过 SDIO 连接模组内的蓝牙控制器。

必须先成功执行 `esp_wifi_init()`，再调用 `nimble_port_init()`，因为蓝牙固件随无线驱动一起加载。仅使用 BLE 时也需要完成 `esp_wifi_init()`。

BluFi 还需启用 `CONFIG_BT_NIMBLE_BLUFI_ENABLE=y`。建议直接使用 [blufi 示例](examples/blufi/)，其中包含官方示例所需的配置与构建适配。

## API 与行为差异

Wi-Fi 应用包含 `esp_wifi.h`，使用标准 `esp_wifi_*` API。当前可用功能包括驱动初始化与启停、STA 连接与断开、扫描结果读取、SoftAP 启停与客户端列表、MAC 地址读写、当前信道读取和省电模式设置。

迁移现有 ESP-IDF 应用时，需要检查以下差异：

| 功能 | 当前行为 |
| --- | --- |
| STA + SoftAP 并发 | `WIFI_MODE_APSTA` 返回 `ESP_ERR_NOT_SUPPORTED` |
| Wi-Fi 配置持久化 | 配置只保存在 RAM；`WIFI_STORAGE_FLASH` 也按 RAM 处理，重启后需由应用重新设置 |
| STA / AP 的 MAC | 两种模式共用同一个 MAC，修改须在 `esp_wifi_start()` 前完成 |
| `esp_wifi_set_channel()` | 返回 `ESP_ERR_NOT_SUPPORTED`；STA 信道由路由器决定，SoftAP 信道通过 `wifi_config_t.ap.channel` 设置 |
| 国家码、最大发射功率读写 | 返回 `ESP_ERR_NOT_SUPPORTED` |
| STA / AP 客户端 AID、按 AID 踢除客户端 | 返回 `ESP_ERR_NOT_SUPPORTED` |
| 混杂模式、CSI、ESP-NOW | 当前组件未提供支持 |

头文件中存在声明不代表对应功能已实现，应用应检查 API 返回值。

### 扩展接口

需要查询无线芯片信息时，包含 [p4_wireless.h](include/p4_wireless.h)：

| 接口 | 用途与限制 |
| --- | --- |
| `p4_wireless_get_fw_version(buf, len)` | 获取无线固件版本，须在驱动初始化后调用 |
| `p4_wireless_get_data_rate(&tx, &rx)` | 获取协商的 PHY 速率，单位为 bps，须在 STA 已关联后调用；当前 `rx` 恒为 `0` |
| `p4_wireless_get_temperature(&temp)` | 温度读数目前不可靠，不应用于温度保护或控制逻辑 |
| `p4_wireless_hci_open()`、`p4_wireless_hci_send()` | 供 HCI 适配层使用，普通 NimBLE 应用无需直接调用 |

PHY 速率用于观察无线链路状态，实际 TCP / UDP 吞吐需单独测量。

## 硬件资源占用

模组内无线芯片使用 SDMMC Slot 1 和四线 SDIO，连接已固定：

| 资源 | 用途 |
| --- | --- |
| GPIO18 / GPIO19 | SDIO CLK / CMD |
| GPIO14 / GPIO15 / GPIO16 / GPIO17 | SDIO D0 / D1 / D2 / D3 |
| GPIO54 | 无线芯片复位 |

应用不要复用这些 GPIO 或 SDMMC Slot 1。使用 SD 卡时应选择 Slot 0，并按开发板配置接线。eFuse `BLOCK_KEY5` 由模组授权占用，不要向该块写入其他密钥。

## 常见问题

| 现象 | 检查方法 |
| --- | --- |
| CMake 提示 `CONFIG_FREERTOS_HZ=1000`、`CONFIG_LWIP_IPV6=y` 或 `CONFIG_LIBC_NEWLIB=y` | 检查当前 `sdkconfig` 的实际值；只修改 defaults 不会覆盖已有配置 |
| CMake 提示 slave 目标不符 | 将 Wi-Fi Remote 的 slave 目标设回 `ESP32C6` |
| `warning: unknown kconfig symbol` | 配置项不属于当前 ESP-IDF 或编译目标，按本页配置检查；ESP32-P4 的 PSRAM 模式使用 `CONFIG_SPIRAM_MODE_HEX` |
| 应用分区过小，出现 `app partition is too small` | 检查生成的固件大小，增大应用分区后重新构建 |
| `esp_wifi_init()` 返回 `ESP_FAIL` | 保留完整启动日志，检查是否为配套模组、供电是否稳定，以及 SDIO / 复位脚是否被占用 |
| NimBLE 输出 `HCI open failed` | 确认 `esp_wifi_init()` 已成功返回，并在其后调用 `nimble_port_init()` |
| API 返回 `ESP_ERR_NOT_SUPPORTED` | 核对上面的功能范围，避免直接沿用片上 Wi-Fi 的全部功能假设 |
| 重启后没有保存 SSID / 密码 | 由应用使用 NVS 等方式保存，启动后重新调用 `esp_wifi_set_config()` |

反馈问题时请附上 ESP-IDF 完整版本、使用的示例或最小复现代码、相关配置、API 返回值和完整启动日志。
