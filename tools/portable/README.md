# 在 Windows、macOS 和其他 Linux 上使用 PXA

[English](README.en.md)

目前完整原生 DevKit 0.5.0 的二进制发行是 Linux x86_64，要求 glibc ≥ 2.39。C++26 Guest 源码不受宿主机平台限制，但 PXA 的固定版本 AOT 编译器和模拟器需要对应宿主机二进制；不能用上游任意 `wamrc` 替换 Espressif Xtensa 后端与 PXA 的固定 WAMR 配方。

本配套提供 **Linux amd64 DevKit 容器**及**宿主机 PXADB**：容器负责构建、签名和无界面模拟器，Windows/macOS 直接打开本机 USB 串口，安装、启动、截图和注入触摸。设备不需要传入容器，也不需要为这个方案运行 USB/IP。这是可执行的跨平台方案，不是 Windows/macOS 原生编译器发行。

## 方式一：容器构建，宿主机调试

安装 Docker Desktop（Windows/macOS）或 Docker/Podman（Linux），以及 Python 3.11+。从配套 Release 下载工具归档和预构建容器归档，先核对签名清单与摘要，再解压工具包、载入镜像：

```sh
docker load --input pxa-devkit-container-0.5.0-linux-amd64.tar.gz
docker tag localhost/pxa-devkit:0.5.0-linux-amd64 pxa-devkit:0.5.0-linux-amd64
python pxa-host-tools-0.5.0/pxa_container.py --workspace . -- init hello
python pxa-host-tools-0.5.0/pxa_container.py --workspace . -- \
  build hello --offline --target esp32s3 --output hello-output
python pxa-host-tools-0.5.0/pxa_container.py --workspace . -- \
  run hello --offline --duration-ms 1500 --screenshot hello.png
```

验证时使用单独可信的 DevKit 公钥（或从已验证的系统源码取得的发行公钥），运行：

```sh
python pxa-host-tools-0.5.0/verify_manifest.py host-tools-0.5.0.json --public-key /path/to/trusted/release-public.pem
```

它先通过 OpenSSL 验证清单签名，再流式检查两个归档的大小和 SHA-256，失败时返回非零。需安装本机 OpenSSL，路径不在 PATH 时加 `--openssl /path/to/openssl`；Windows 也可在 WSL 中执行验证。包内公钥副本方便查阅，不能作为首次下载的独立信任来源。验证成功后再载入镜像与运行工具。

归档由 Podman 导出，载入后的镜像名称带 `localhost/` 前缀。上面的 `docker tag` 为包装器添加默认名称，Docker 和 Podman 都应执行；该操作只增加名称，不复制镜像层。使用 Podman 时，将两个 `docker` 命令换为 `podman`，并给包装器添加 `--engine podman`。

Windows PowerShell 可将长命令写在一行，或用反引号续行；上面的反斜杠续行用于 POSIX shell。`--workspace` 是包含应用及共享 `common/` 的父目录，命令内路径相对于该目录。带空格路径可用引号。容器使用 SDL dummy，图像写到宿主机，可直接打开 `hello.png`；它不会自动弹出桌面窗口。

`.pxa-container/data` 保留开发身份及锁定发行，`.pxa-container/cache` 保留构建资源缓存；项目的 `.pxa` 也保留增量构建。不要删除或公开身份目录。可用 `--state <目录>` 指定外部位置，用 `--dry-run` 查看参数。Linux 使用宿主 UID/GID，Podman 使用 keep-id；Apple Silicon 和 ARM Windows 使用 `linux/amd64` 仿真，构建会比对应原生二进制慢。

宿主机 PXADB 无编译器依赖。创建虚拟环境并安装串口、截图依赖，以下是 Windows PowerShell 命令：

```powershell
py -3 -m venv .pxadb-venv
.\.pxadb-venv\Scripts\python.exe -m pip install -r pxa-host-tools-0.5.0\requirements.txt
.\.pxadb-venv\Scripts\python.exe pxa-host-tools-0.5.0\pxadb.py devices
.\.pxadb-venv\Scripts\python.exe pxa-host-tools-0.5.0\pxadb.py info --port COM7 --baud 2000000
.\.pxadb-venv\Scripts\python.exe pxa-host-tools-0.5.0\pxadb.py package install hello-output.pxa --yes --port COM7 --baud 2000000
.\.pxadb-venv\Scripts\python.exe pxa-host-tools-0.5.0\pxadb.py package run pxa-hello --port COM7 --baud 2000000
.\.pxadb-venv\Scripts\python.exe pxa-host-tools-0.5.0\pxadb.py screenshot hello-device.png --port COM7 --baud 2000000
```

输出包路径以 build 实际输出为准；上例 `--output hello-output` 对应 `hello-output.pxa`，`pxa init hello` 的默认 ID 是 `pxa-hello`。把 COM7 换为 `devices` 实际列出的端口，串口桥设备必须匹配波特率。安装会在设备预览验证信息，`--yes` 仅代表用户明确接受本次安装，不会绕过签名验证。

macOS/Linux 将虚拟环境解释器换为 `.pxadb-venv/bin/python`、端口换为 `/dev/cu.usbmodem...` 或 `/dev/ttyACM...`：

```sh
python3 -m venv .pxadb-venv
.pxadb-venv/bin/python -m pip install -r pxa-host-tools-0.5.0/requirements.txt
.pxadb-venv/bin/python pxa-host-tools-0.5.0/pxadb.py devices
.pxadb-venv/bin/python pxa-host-tools-0.5.0/pxadb.py info --port /dev/cu.usbmodem123 --baud 2000000
```

截图、触摸、日志和包管理始终使用同一个宿主机客户端。一个串口同时只能由一个工具占用；先停止固件监视器或 GUI 再运行 CLI。Linux 用户需要串口访问权限。容器包装器拒绝 `run --device`，并提示使用本机 PXADB。

若要自己构建相同镜像，在空目录中将**已验证且完整**的原生 DevKit 放到 `devkit/`，复制本目录 Dockerfile 后执行：

```sh
docker build --platform linux/amd64 --tag pxa-devkit:0.5.0-linux-amd64 .
```

它直接复制已发布二进制，不下载 LLVM、不重新编译。镜像和原生 DevKit 使用相同应用 ABI、工具版本和签名流程。

## 方式二：Windows WSL 2

可在 WSL 2 的 Ubuntu 24.04 使用原生 Linux DevKit；项目放在 Linux 文件系统更利于增量构建。设备调试优先使用前述 Windows PXADB，以免独占 USB。

需要在 WSL 内直接访问 USB 时，Microsoft 提供 usbipd-win 路线：管理员 PowerShell 执行 `usbipd list`、`usbipd bind --busid <ID>`；正常 PowerShell 执行 `usbipd attach --wsl --busid <ID>`。WSL 中重新确认 `/dev/ttyACM*`/`ttyUSB*`，结束后 `usbipd detach --busid <ID>`。设备附加期间 Windows 不能同时使用；拔插或复位后可能需要重新附加。具体先决条件与当前命令见 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)。

Docker Desktop 的 USB/IP 也有官方路径，但需要额外配置；本配套不依赖它。[Docker 文档](https://docs.docker.com/desktop/features/usbip/)

## 原生工具链与自行构建

WASI SDK 34 已提供 Windows/macOS/Linux 多架构归档，[上游发行页](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-34) 是 C++→Wasm 编译器来源，**不包含 PXA 的设备 AOT 与 Host**。

需要完整原生 PXA 开发环境时，必须完成以下部分：

1. 与 `deps/pxa-system/config/wamr.json` 一致的 WAMR 和 Espressif LLVM 提交，并保留 Xtensa、RISCV 等目标后端；不能只用发行标签猜版本。
2. 对应平台 WASI SDK 34、CMake、Ninja、OpenSSL 与 Python，及 `pyserial`、Pillow。
3. 对应平台的 PXA 模拟器和包安装器，以及相同固定 LVGL、音频、压缩、字体等依赖。
4. 移植工具脚本的 POSIX 锁、进程组、Unix socket、动态库和平台路径，再执行真实 Guest、包验证及输入回归。

当前 Linux x86_64 的可重复维护者构建入口是 `tools/release.py`，使用固定容器与隔离 LLVM/WAMR 缓存；底层 AOT 配方在 `deps/pxa-system/tools/package/build_wamrc.sh`。已经完整验证的是 Linux 配方；不能将它原样贴到 PowerShell 当作原生 Windows 构建步骤。macOS/ARM 的原生开发包尚未完成整套验证，因此本轮提供容器方案及宿主机调试客户端。

## 验证范围

容器内的离线初始化、ESP32-S3 / ESP32-S31 / x86_64 打包与真实 Guest 截图，以及宿主机 pai-touch 的 INFO、截图与输入会保留实际证据。Windows/macOS 原生机器与 USB 驱动若未实测，会在发行说明中列为未验证；不把 Linux 的串口测试外推为所有操作系统通过。
