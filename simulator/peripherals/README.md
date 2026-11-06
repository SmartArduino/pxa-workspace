# 桌面相机 Host

工作区模拟器通过可选的原生 Host 扩展接入电脑摄像头，复用
`org.doit.peripherals` 0.7 的权限、档位、JPEG 流与相册视频帧接口。
不改变 App 的 Guest ABI，也不把工作区扩展实现放入 PXA Core。
`tools/simulator.sh` 配置、构建桌面模拟器时默认包含此提供者。
独立 Core 构建不指定 `PXSYS_DESKTOP_HOST_EXTENSIONS_DIR` 时不链接它。

Linux 使用系统 V4L2 接口，自动选择第一个支持采集、MMAP 及 MJPEG/YUYV
的摄像头，跳过元数据节点。档位来自实际硬件枚举；目前限制在1280×720以内，
默认优先320×240。只查询能力不会开始采集；应用持有声明的 `camera.jpeg`
权限且在前台时才启动流。桌面开发 Host 沿用声明权限自动授权的现有策略。
实际采集、格式协商和关闭在独立线程进行，UI 线程只轮询完成状态。
后台、关闭应用、撤权或退出时释放设备与映射。设备被其他程序占用时返回忙，
不会停止或抢占其他程序。其他桌面平台目前不提供原生摄像头端口。

优先转交原生 MJPEG，YUYV 设备使用现有 JPEG 库编码，遵循驱动报告的
全范围/有限范围及BT.601/BT.709色彩信息。预览复用 Host 的图像流解码，
无需把整帧 JPEG 交给 Guest 再解码。录像保存实际时间戳，帧率取决于摄像头、
图像解码和磁盘速度；不会插入重复帧来宣称满帧。

可选配置：

```sh
# 指定摄像头；off 禁用硬件采集，相册视频帧接口仍可用。
PXA_SIM_CAMERA_DEVICE=/dev/video0 tools/simulator.sh ui --profile generic
PXA_SIM_CAMERA_DEVICE=off tools/simulator.sh ui --profile generic

# 诊断时强制使用一种输入格式；默认自动优先 MJPEG。
PXA_SIM_CAMERA_FORMAT=yuyv tools/simulator.sh ui --profile generic

# 桌面应用私有文件的磁盘配额，默认256MiB，最多2GiB，不预占内存。
PXA_SIM_PRIVATE_FS_BYTES=536870912 tools/simulator.sh ui --profile generic
```

运行普通 `pxsys_desktop_camera_test` 检查能力边界、分块流所有权、取消、颜色
转换；`pxsys_desktop_camera_test --camera` 另行使用真实摄像头，检查反复打开、
采集、关闭后，工作线程仍存在时设备、映射及内存预算均已释放。

实现只调用系统导出的 V4L2 UAPI，不链接libv4l或FFmpeg。系统
`videodev2.h`、`v4l2-controls.h` 选择明确提供的BSD-3-Clause分支；系统通用
UAPI类型头受Linux-syscall-note约束，仅使用用户态接口声明，不引入内核实现。
JPEG 编码使用桌面 Host 已有的 IJG/libjpeg-turbo 依赖。保留发行系统的头文件
及JPEG库版权声明，不将这些许可解释为允许引入其他copyleft实现。
