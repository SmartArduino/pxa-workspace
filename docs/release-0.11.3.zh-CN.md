# PXA 固件工作区 0.11.3

本补丁修正 LittleFS 空间统计。字库等大文件被多个只读句柄访问时，驱动原来的
近似统计可能重复计算同一批数据块，导致设备信息、商城和安装预检误报空间不足。
新的统计按物理块号去重，同时保留仍被打开的旧文件版本所引用的独立数据块，
不会把这些尚未释放的块算作空闲。

适配器在构建目录生成，保留锁定的 LittleFS 源码与许可声明。统计只读取文件系统；
内存分配或块遍历失败时保守报告没有可用空间，并输出诊断日志。它不会删除、压缩
或改写应用、字体和用户数据，也不改变安装签名、权限或发布者身份检查。

System / DevKit 继续使用 0.11.1、Guest Core 2.1 和 Host C ABI 2。
设备升级前核对板型与分区表，只写入匹配的应用分区以保留数据；完整出厂镜像会
覆盖数据分区。此前 0.11.2 的 PXADB 回复完整性修复继续保留。

## English

This firmware patch corrects LittleFS space accounting when multiple readers
retain the same large file. Unique block accounting avoids duplicate CTZ-chain
counts while retaining distinct blocks held by older open file versions.
The build generates a reviewed adapter without modifying the installed driver.
Allocation or traversal failure conservatively reports no available space.

The query does not alter filesystem data or installation trust checks. System /
DevKit remains 0.11.1 with Guest Core 2.1 and Host C ABI 2. Match the board and
partition map before an application-only update; full factory images replace
the data partition. Firmware 0.11.2's PXADB integrity changes are retained.
