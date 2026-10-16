# ESP32-S3 JPEG 解码对照

独立诊断固件，对比 ROM TJpgDec 和 `esp_new_jpeg` 的整图、缩放、分块输出。
相同压缩输入放在 PSRAM，最终输出均为 PSRAM 中的 RGB565 LE。
每组连续执行 8 次，记录耗时、输出校验、与 ROM 的平均颜色差、局部 SRAM/PSRAM 峰值和泄漏。
固定输出缓冲和参考图片在测量窗口外分配；报告另列有效输出字节数。
因此这些数字是**解码器工作区与 CPU 数据，不是浏览器内存总量或 LCD FPS**。

需要已下载主固件依赖、ESP-IDF 环境和 Pillow。工作目录为 pxa-workspace 根目录：

```sh
source /path/to/esp-idf/export.sh
python firmware/components/pxa/tests/jpeg_benchmark/generate.py /tmp/pxa-jpeg-fixtures
flock .pxa/firmware-build.lock idf.py \
  -C firmware/components/pxa/tests/jpeg_benchmark -B /tmp/pxa-jpeg-build \
  -DIDF_TARGET=esp32s3 -DJPEG_FIXTURE_DIR=/tmp/pxa-jpeg-fixtures build
python firmware/components/pxa/tests/jpeg_benchmark/run_device.py \
  --port /dev/serial/by-id/YOUR_DEVICE \
  --build-dir /tmp/pxa-jpeg-build \
  --restore-binary /path/to/device-matching-production-app.bin \
  --output /tmp/pxa-jpeg-results
```

先准备与设备当前版本、分区布局一致的生产应用固件备份。脚本只写应用地址 `0x10000`，
不写 bootloader、分区表、应用包或数据；结束和异常时均在 `finally` 中恢复生产应用固件。
它不能防止断电导致恢复中断，因此应保留恢复文件和刷写日志。

可选传入 `-DPXA_IMAGE_BASELINE_FILE=/absolute/path/to/saved/image_decode.c`，
将保存的基线与当前公共像素处理代码编入同一诊断固件。输出额外的 `blit-results.json`：
覆盖 RGB888 MCU（1、1/2、1/4 采样）、RGB565、普通和 Adam7 风格 RGBA 点采样。
两版使用同一个输入、输出尺寸、分配器和目标对象；每组预热一次，再测 8 次。
任何输出校验差异或未释放的预算会使诊断中止。

默认不编入这项附加测试。诊断固件、固定大缓冲及测试依赖均不会进入生产固件。
