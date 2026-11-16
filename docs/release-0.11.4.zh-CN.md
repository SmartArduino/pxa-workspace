# PXA 固件 0.11.4

本版配合 System / DevKit 0.11.2、Host SDK 0.11.2+idf.1、Agent Kit 0.7.5
和 Browser Host 0.11.3。
Guest Core 2.1、Host C ABI 2 与现有 AOT ABI 保持兼容。

## 按预算查询图片输入能力

Assets 2.7 增加可选的图片输入预算查询。应用提供输出像素预算，Host 返回后端
编码输入上限、当前可用建议值与动画输入上限。旧 Host 不支持查询时，应用仍需
保留原有 512 KiB 回退路径。

静态编码输入的校验上限为 16 MiB；这个数字不代表设备总能接收相同大小的图片。
建议值受全局资源、应用额度、临时内存及实际可分配空间约束，查询不预留内存。
申请时仍会检查预算，应用必须处理资源不足、取消及格式不支持。默认像素预算和
默认 128 KiB 编码输入配置不变，GIF 动画编码输入仍最多 512 KiB。

`CONFIG_PXA_RESOURCE_PSRAM_AUTO` 默认根据检测到的 PSRAM 设置资源总额度和临时
额度。例如 8 MiB 对应 4 MiB / 2 MiB，32 MiB 对应 16 MiB / 8 MiB；额度不会提前
占用内存，实际申请还要保留系统所需的空闲空间。关闭此配置可沿用手工额度。
未扩展的整图解码后端仍使用其原有输入上限。

## 板级操作描述与升级

GameKit 2 增加与实际输入路由对应的操作描述，区分触摸、手柄、原生焦点和游标
路径，并声明 MENU/RETURN 系统保留操作。FT6336U 的物理路由决定触点数，调试
接口的五点注入不代表五点硬件。未验证的长按或跨模式并发能力保留为未知。

Wi-Fi 在已有连接断开后会正确标记后续重连过程，继续处理失败事件；配置或启动
连接的即时错误会重新安排扫描，避免恢复流程停住或因驱动错误中止系统。

七款维护中板型提供独立 app、首次安装完整镜像和 ZIP，首次安装镜像包含中文
字体与商城 0.9.8。已有设备应先匹配板型、芯片与真实分区表，再进行 app-only
升级，以保留现有应用、存档、字体与设置。独立产品需使用对应 Host SDK 重新
构建自己的固件，不能套用其他板型镜像。

P4 的 UART-DEBUG 链路仍有已知控制回复完整性限制，本版不包含未经实机验收的
UART 节流配置。刷写后必须验证镜像与启动结果；单独降低主机串口波特率不会
改变正在运行的固件配置。

## English

Firmware 0.11.4 pairs with DevKit 0.11.2 and Agent Kit 0.7.5. Assets 2.7 lets
applications query current image-input budgets while retaining the 512 KiB
fallback on older Hosts. The 16 MiB validation ceiling is not a memory promise;
allocation checks, output limits and the 512 KiB GIF ceiling remain enforced.
PSRAM quotas adapt to detected capacity, with runtime heap checks and a manual
configuration option. Match the board and partition layout before app-only
updates; use complete factory images only when initializing or migrating data.
