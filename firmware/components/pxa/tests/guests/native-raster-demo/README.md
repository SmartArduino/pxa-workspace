# 月岭天文台 · PXA 3D 光栅压测

Demo 按显示器原生像素创建 GameRender 目标，使用深度测试、透视贴图、镂空和半透明效果。暮色雪峰前的天文台由铜与青釉浑天仪、月石星盘、立柱和镂空栏杆组成，发光粒子沿浑天仪飞行。镜头每 60 个成功提交的帧切换一次，依次环绕、贴地穿行和俯视旋转。每一轮使用相同的逐帧镜头位置，便于对比不同的光栅实现。

从工作区根目录构建 ESP32‑S31 和桌面模拟器通用的开发包。使用工作区的 WASI SDK 和 WAMRC 配置。

```sh
out="local/artifacts/native-raster-demo/$(date +%Y%m%d-%H%M%S)"
tools/app.sh build native-raster-demo --board esp-mosaico --target esp32s31,simulator \
  --source-root firmware/components/pxa/tests/guests --output "$out/package"
tools/simulator.sh product --package "$out/package/pxa-native-raster-demo" \
  --publisher-key deps/pxa-system/apps/pxa/.dev-signing/publisher-public.der \
  --width 480 --height 480
```

`art/material-atlas.png` 和 `art/twilight.png` 分别是材质和远景的编辑源图。更新源图后，用装有 Pillow 的 Python 重建 `art/baked_textures.hpp`。包内使用烘焙后的 INDEX8 贴图和共享 RGB565 调色板。

```sh
python3 firmware/components/pxa/tests/guests/native-raster-demo/art/bake_textures.py
```

宿主回归使用同一场景、贴图、镜头和 Guest 命令编码器。它逐帧比较整屏与分段光栅的 RGB565、深度平面和覆盖像素数。在 C++26 宿主上运行以下命令。设置 `PXA_NATIVE_DEMO_BASELINE` 为 PXA 系统子模块提交，可在同一进程逐帧比较两套光栅实现。

```sh
bash firmware/components/pxa/tests/test_native_raster_demo.sh
```
