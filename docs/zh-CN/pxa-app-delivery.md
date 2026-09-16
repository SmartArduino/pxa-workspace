# PXA 应用交付

[English](../pxa-app-delivery.md)

本工程将固件与 PXA 应用分为两条独立交付链路。ESP-IDF 构建只编译固件，不会编译
PXA 应用、生成 LittleFS 镜像，也不会把 PXA 数据分区加入 `idf.py flash`。因此常规
固件烧录会保留已安装的应用和用户数据。

每块板子都要在以下两个位置配置 PXA 数据存储，分区标签必须完全一致：

```text
firmware/boards/<board>/partitions.csv
  <label>, data, littlefs, ...

firmware/boards/<board>/sdkconfig.defaults
  CONFIG_PXA_STORAGE_PARTITION_LABEL="<label>"
  CONFIG_PXA_MOUNT_POINT="/<mount>"
  CONFIG_PXA_BUILTIN_PACKAGE_ROOT="system/pxa/factory"
```

`pai-touch` 使用 `pxa_data` 分区标签和 `/pxa` 挂载路径。名称由板子决定，`assets`
仅是通用兼容默认值，并不是固定要求。

## 通过 pxadb 开发交付

在被忽略的 `local/apps.toml` 注册 App 的外部源码根目录，再独立打包应用：

```sh
tools/app.sh build <app-id> --board pai-touch
```

命令输出如下：

```text
local/app-output/pai-touch/pxa-<app-id>/
local/app-output/pai-touch/pxa-<app-id>.pxa
local/app-output/pai-touch/pxa-<app-id>.pxa.provenance.json
```

使用 `.pxa` 单文件容器进行 pxadb 安装。客户端将其上传到
`<CONFIG_PXA_STATE_ROOT>/inbox/`，随后对该应用身份发起 PXA package `install`
操作。固件完成验签并将应用提交至受管理的 Package 目录。之后重新编译或烧录固件，
不会重新安装或删除该应用。

先安装工作区内的主机客户端，再将 App 部署到设备：

```sh
python3 -m pip install -e tools/pxadb
pxadb package install local/app-output/pai-touch/pxa-<app-id>.pxa \
  --port /dev/ttyACM0
```

`pxadb package install` 同时完成暂存和安装。非交互式替换已安装 App 时使用 `--yes`；
`pxadb package list` 可查看最终 Package 状态。主机客户端通过所选板子的 PXADB USB
Serial/JTAG 服务通信，不依赖其他固件仓库。

一次性打包可使用 `--source-root <root>`，无需写入本机 catalog；`PXA_SIGNING_KEY`
用于选择签名私钥。App 源码和输出都不必放入本仓库，且打包命令不会调用 ESP-IDF。

## 出厂分区镜像

出厂 Profile 与 App 源码路径分离管理。需要出厂预置时，显式生成完整的 PXA 数据分区镜像：

```sh
tools/factory.sh image pai-touch
```

默认输出路径为：

```text
out/pxa-partitions/pai-touch/pxa_data-empty.bin
```

在 `factory/profiles/pai-touch.toml` 中加入正式发布的 App ID；对应源码根目录从
`local/apps.toml` 解析。私有或临时 App 使用被忽略的 overlay：

```sh
tools/factory.sh image pai-touch --overlay local/factory-pai-touch.toml
```

可通过 `--output <image>` 指定其它输出路径。工具会读取板子的分区标签、偏移、大小、
LittleFS 文件名长度和出厂 Package 根目录，并输出对应的烧录偏移。出厂 App 会被放入
`CONFIG_PXA_BUILTIN_PACKAGE_ROOT`；当前 pai-touch 的受控 Profile 默认包含产品字体。

烧录该镜像属于出厂置备，因为它会替换整个 PXA 数据分区。不要将它用于已有已安装应用
的设备日常固件升级或应用开发，应使用 pxadb。

若未置备出厂镜像，默认的 `CONFIG_PXA_STORAGE_FORMAT_IF_MOUNT_FAILED=y` 会在首次启动
时格式化尚未初始化的 PXA 数据分区，建立空的安装区以便 pxadb 安装应用。若产品要求
在挂载失败时保留现场而不是自动格式化，应关闭该选项。
