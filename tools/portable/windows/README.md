# Windows 原生 PXA 开发工具

本目录提供 Windows x64 源码构建入口，生成标准 UI 模拟器、Guest 运行器、包安装器和带 Xtensa/RISC-V 后端的固定版本 `wamrc.exe`。应用 CLI 使用 `deps\pxa-system\bin\pxa.cmd`，PXADB CLI/GUI 继续使用本机 Python 和 COM 串口。

最低系统为 Windows 10 1903，推荐 Windows 11 x64。当前构建使用原生 MinGW-w64（包括 MSYS2 UCRT64 环境里的原生编译器）；不支持以 MSVC 编译整套 Host。MSYS2 只用于安装开发环境，生成的程序不链接 `msys-2.0.dll` 或 `cygwin1.dll`。

## 准备

安装 python.org 的 Windows x64 Python 3.12+，以及 Git、CMake 3.19+、Ninja、pkg-config 和 MinGW-w64 x64。它们必须在运行构建命令的终端 PATH 中。`gcc -dumpmachine` 应包含 `mingw`。建议将源码放在较短路径，例如 `C:\pxa\src`，为 LLVM 和依赖的构建目录留出路径长度。

例如在 MSYS2 的 UCRT64 终端中准备原生编译环境：

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf
```

使用 PowerShell 构建时，将 MSYS2 的原生工具和 Git 加入当前终端 PATH（按实际安装位置调整）：

```powershell
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:Path"
```

在仓库根目录初始化源码，创建并激活虚拟环境后安装依赖：

```powershell
git submodule update --init --recursive
python -m venv .pxa-tools/windows-python
$env:Path = "$PWD\.pxa-tools\windows-python\Scripts;$env:Path"
python -m pip install -r tools/portable/requirements.txt
```

另行下载并校验 [WASI SDK 34 的 Windows x86_64 归档](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-34)，解压到例如 `C:\pxa\wasi-sdk-34`。该目录应包含 `bin\clang++.exe` 和 `share\wasi-sysroot`。

## 构建

在仓库根目录运行（PowerShell 或 cmd.exe）：

```powershell
python tools/portable/windows/build.py --wasi-sdk C:\pxa\wasi-sdk-34 --jobs 8
```

首次构建下载固定版本的 vcpkg 依赖、LVGL 以及 `config/wamr.json` 指定的 Espressif LLVM，LLVM 编译需要较长时间和较多磁盘空间。WAMR 与 LLVM 提交会核对；不使用任意上游 `wamrc`。构建执行 Windows 文件系统检查和模拟器 smoke/self-test；提供 `--wasi-sdk` 时，还会用独立测试身份构建包含 Windows、ESP32-S3、ESP32-S31、ESP32-P4 AOT 的 Hello 包，并在模拟器中实际运行、截图。默认输出全部在被 Git 忽略的 `local/artifacts/windows-native/<时间>/`。

可通过 `--output` 重用同一构建目录；`--simulator-only` 单独构建模拟器。已有 vcpkg 可用 `--vcpkg-root`，其 HEAD 必须匹配 `vcpkg.json` 的 baseline，构建器不会重置已有检出。`--prefix` 接受自行审查、编译好的 Windows 依赖；该前缀须包含 SDL2、JPEG、PNG、FreeType、curl、OpenSSL、LZ4、Ogg、Vorbis、Opus、opusfile 的开发文件和 pkg-config 元数据。

构建完成会打印 `env.cmd`。在 **cmd.exe** 中载入它，再调用源码 CLI：

```bat
call "local\artifacts\windows-native\<时间>\env.cmd"
deps\pxa-system\bin\pxa.cmd init hello
deps\pxa-system\bin\pxa.cmd run hello --offline
deps\pxa-system\bin\pxa.cmd build hello --target esp32s3 --offline
deps\pxa-system\bin\pxadb.cmd devices
```

GUI 可在上述虚拟环境中独立安装并启动：`python -m pip install -e tools/pxadb -e tools/pxadb-gui`，
然后运行 `tools\pxadb-gui.cmd --port COM7`，或连接已经启动的 `run --debug` 模拟器。
GUI 的 PySide6 依赖独立于系统 Host 运行时。

`--target simulator` 在 Windows 自动选择 Windows x64 AOT。Linux/macOS 构建机可以用 `--target windows-simulator` 生成 Windows 模拟器包。两种 AOT 的调用约定不同，包中的目标分别是 `windows-x86_64` 与 `linux-x86_64`，不能混用。设备目标不受宿主操作系统影响。

Windows CLI 输出已签名的 `.pxa` 文件和对应 provenance/build 元数据，不依赖目录符号链接权限。开发签名身份保存在 `%LOCALAPPDATA%\pxa\identities`（显式 `XDG_DATA_HOME` 优先）。不要公开身份目录。`run --debug` 使用 [Windows 本机 AF_UNIX](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/)，经 WinSock 接入 PXADB；无需开放 TCP 端口。

## 依赖与平台语义

`vcpkg.json` 固定依赖基线并关闭未使用的默认特性。curl 选择系统 Schannel/SSPI，关闭 PSL、IDN2、GnuTLS 等可选后端；FreeType 选择 FTL 分支，关闭可选依赖；音频仅使用 BSD 许可的 Ogg/Vorbis/Opus。OpenSSL 使用 Apache-2.0。分发二进制时须随包保留实际依赖的版权、FTL 归属声明、编译器运行库及其已核实的运行时例外，并审查实际 DLL 导入闭包；不能把整个开发环境的 DLL 目录直接装进发行包。

Host 文件系统使用 `NtCreateFile` 的目录句柄相对访问，拒绝重解析点、路径越界和 ADS；文件锁使用 Win32 字节锁。文件数据会在发布前 flush。Windows 没有等价的用户态目录 `fsync`，掉电后的目录持久性由文件系统管理。此差异不应作为 Windows 上完整掉电恢复保证。

Linux 到 Windows 的交叉编译工具链为 `deps/pxa-system/cmake/windows-mingw-x64.cmake`；交叉构建需提供对应 Windows 依赖前缀、宿主 Python 和 pkg-config。交叉编译生成的 PE 文件可在 Wine 中验证，仍需 Windows 实机确认窗口、音频、COM 驱动和完整首次构建流程后再发布正式 Windows DevKit。

## 发布预编译包

`assemble.py` 接收已构建的模拟器、固定配方 wamrc、Windows WASI SDK 34、嵌入式 Python、CMake/Ninja、OpenSSL、审查过的 DLL 与许可目录，生成五个拆分组件、完整 ZIP 和签名 `distribution-windows-x86_64.json`。它按 PE 导入表递归收集 DLL，不打包 MSYS/Cygwin。源码必须已提交，输出目录必须为空。签名私钥通过 `--signing-key` 从工作区外传入。必须先验证原始依赖归档的官方摘要并保留版权声明，实际运行解压后的包，再上架；`--validation wine` 明确表示尚未完成原生 Windows 实机验收。

打包前安装本目录 `requirements.txt` 中固定的 LIEF（Apache-2.0，仅用于构建）。打包器为未签名的 OpenSSL 合并 UTF-8 进程清单，使中文用户名和项目目录可用于签名；同时核对代码、入口、版本及版权资源保持一致。已带 Authenticode 签名且未启用 UTF-8 的 OpenSSL 会被拒绝，应提供自行构建并启用 UTF-8 的版本。

下载 ZIP 后解压，将 `bin` 加入 PATH，可用 `pxa init hello` 与 `pxa run hello --offline`，无需安装 MSYS2 或编译 LLVM。包内 Python 负责普通 CLI；Agent Kit 使用其 uv 固定 Python 环境，并自动下载对应平台签名组件。

Windows 工具链还需要微软 Visual C++ 2015–2022 x64 运行库；电脑未安装时，从 [微软官方下载](https://aka.ms/vs/17/release/vc_redist.x64.exe) 安装一次，再运行 setup。AI 会在下载原生开发包前检查并说明此项。发行包不会混入 Wine 的运行库实现。
