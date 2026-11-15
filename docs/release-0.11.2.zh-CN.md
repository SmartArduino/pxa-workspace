# PXA 固件工作区 0.11.2

本补丁修复共享控制台下的 PXADB 回复完整性。Newlib 固件在一整条回复发送期间
锁定标准输出与标准错误流，避免普通日志或诊断输出插入协议分块。这样可减少
设备信息、兼容性预检和截图传输中的偶发格式错误。

ESPP4-A 的 UART-DEBUG 保持 2,000,000 波特率，默认以 32 字节突发发送回复，
每次排空后间隔 100 微秒，给无硬件流控的 USB 串口桥接收缓冲留出余量。
该设置会降低连续回复的吞吐量。自适配板可调整
`CONFIG_PXADB_UART_TX_BURST_BYTES` 与 `CONFIG_PXADB_UART_TX_GAP_US`；突发长度
为 `0` 时关闭节奏控制。原生 USB 沿用其发送路径。

固件与系统 SDK 独立编号。本补丁继续使用 PXA System / DevKit 0.11.1、Guest
Core 2.1 和原生 Host C ABI 2，已有应用及工具无需因固件补丁重新构建。
保留数据升级前须核对板型和分区表，并只写入对应应用分区；完整镜像会覆盖
数据分区，适用于首次安装或经过备份的布局迁移。

## English

This firmware patch protects complete PXADB replies against concurrent Newlib
console output. ESPP4-A retains its 2 Mbps UART rate and uses drained 32-byte
bursts with a 100 microsecond gap for bridges without hardware flow control.
Pacing trades continuous reply throughput for receiver headroom; native USB
keeps its existing write path. Other boards can configure these UART options.

PXA System / DevKit remains 0.11.1 with Guest Core 2.1 and Host C ABI 2. Match
the board and partition map before an application-only update that preserves
data. Merged factory images replace the data partition.
