# Wi-Fi Location 0.1

用于网络定位的可选 Host 扩展，工作区分配为服务 33027，身份 UUID
`2bdd0e91-e1c4-48da-99e2-6a70f3113985`。它不是 PXA 标准服务。
协议版本 0.1；SDK 先核对 HELLO 身份与版本，再请求扫描快照。

`HELLO`（opcode 1）：请求与结果均为身份 16 字节、major:u16=0、minor:u16=1。
`SNAPSHOT`（opcode 2）：请求为 `permission:u64`，必须属于调用 Component，
且匹配 `org.doit.wifi-location` / `wifi.scan`。每次请求重新校验授权。
结果为 `timestamp_us:u64 | count:u16 | (BSSID:6 bytes | RSSI:i8 | ssid_bytes:u8 | SSID)*count`，
最多十个，单个 SSID 为 1..32 字节。
所有多字节整数为小端，时间戳是 Host 单调时钟中的实际扫描时刻。
返回快照是一次拷贝，没有跨请求资源或长期保留的权限句柄。

后端回调必须非阻塞。ESP 后端在独立短生命周期 FreeRTOS 任务中调用现有
Wi-Fi 扫描器，期间返回 WOULD_BLOCK；不在 Runtime/LVGL 线程等待扫描。
成功和失败结果限速缓存十秒。较旧扫描结果不伪造新时间；应用还须检查时效。
未收集到至少两个可用热点时返回 UNAVAILABLE。

ESP 后端过滤组播/本地管理地址和 `_nomap` 热点，返回 BSSID、RSSI 与 SSID，
不返回密码、设备自身 MAC 或完整扫描列表。SSID 用于服务端对混淆的热点记录
进行匹配。Guest 跳过非 UTF-8 名称。本扩展不请求任何外部定位服务。
应用负责告知所选服务、申请网络权限、提交信号并判断结果精度。

构建选项 `PXA_ENABLE_WIFI_LOCATION` 控制固件接入，只有 pai-touch 默认开启。
其他板型需确认使用相同的 Wi-Fi 管理器后显式启用。关闭后应用的其他定位来源
仍可使用；旧固件会在 HELLO 返回不支持。无查询时不分配扫描任务或发起扫描。

在工作区根目录执行原生权限/协议测试：

```sh
cmake -S extensions/wifi-location -B "$artifact_dir/wifi-location"
cmake --build "$artifact_dir/wifi-location"
ctest --test-dir "$artifact_dir/wifi-location" --output-on-failure
```

`artifact_dir` 应指向被忽略的 `local/artifacts/<topic>/<时间戳>`。
实现与 SDK 为 Apache-2.0；复用现有 Wi-Fi 驱动，不引入新的第三方运行库。
