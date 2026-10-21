# PXA 应用身份与可读存储目录：实现与接入

本次实现包含设备 Host、POSIX 安装器、Core FS、C/C++ SDK、商店应用及 app_store_backend。没有方案展示页面。实际发布者管理表单位于服务器开发者后台。

## 身份与目录

应用唯一身份仍是 `lineage_root_key_id:app_id`。根密钥来自已验证签名谱系。作者昵称、登录账号、当前签名子密钥、目录名均不能替代它。更新时会在安装事务准入处再次核对预期完整身份，避免预览后更换包而更新其他作者的同名应用。

包源文件支持：

```json
"publisher": {"handle": "lucinhu", "display_name": "Lucinhu"}
```

打包器将此字段写入 `assets/pxa-publisher.json`，纳入文件清单和签名。设备只从已经验证的 incoming 文件读取该描述。`handle` 为 1–24 字节小写 ASCII 标识；展示名可以包含中文。昵称不构成商店认证。

没有作者描述、也没有现有认证绑定的旧包继续使用原目录，不会先占用一个通用作者名。以后升级到带作者描述的包时才分配并迁移；已经绑定的路径不会因后续包省略描述而重置。

常见路径：

| 内容 | 路径示例 |
| --- | --- |
| 安装包 | `/pxa-state/packages/lucinhu/pxa-nes/` |
| 私有数据 | `/pxa-state/data/lucinhu/pxa-nes/` |
| SD 卡媒体 | 卡根下 `/PXA/media/lucinhu/pxa-nes/` |

受保护注册表将目录绑定到完整身份。离线作者命名空间按已验证的谱系根隔离。不同作者声明相同昵称时，后分配目录增加 `--` 与 Base32 指纹；冲突时逐步延长，绝不按昵称合并数据。商店在线注册的重复 handle 返回 HTTP 409。历史绑定保持稳定；更新作者描述不会把已有目录转给其他身份。

当前设备实现预分配 16 个作者、32 个应用记录，约 7 KiB，存于 NVS `pxa_apps/locators_v1`。记录保留以保护卸载后及未插入 SD 卡上的数据。达到容量返回资源限制；不会回收其他应用路径或降级为不安全匹配。扩大容量、注销数据的生命周期策略应随具体产品存储预算另行配置。

## 启动、更新与迁移

Store Installer 0.7 保留旧操作码，新增 9/10（完整身份启动/卸载）、11（注册认证作者绑定）、12（固定预期谱系根的安装）。短 app_id 仅在唯一匹配时兼容；重名返回错误。

POSIX 安装器通过可选配置尾部回调解析 package/data 目录；旧结构尺寸调用者仍使用原路径。owner、lock、session 等控制记录继续使用完整身份。Core 恢复丢弃未提交 incoming、回滚 previous，不将未提交包提升为已安装包。

旧目录在停止应用后迁移：先持久化意图，再作非覆盖 rename，最后持久化完成状态。各阶段可重入；掉电后在启动恢复或再次激活时收尾。目标已存在且没有对应意图时拒绝合并。注册表损坏或提交结果不确定时关闭相关操作，直到重新加载确认。私有文件不复制、不重建。

SD 未就绪不阻止运行 Flash 应用；首次打开媒体根之前重试旧哈希目录迁移。只有迁移完成后才颁发新的媒体根。查询显示路径不会挂载 SD、触发授权或打开目录。

## 获取 SD 显示路径

FS 0.4 的可选特性为 `app-media-path`，操作码 18。C SDK 使用 `pxa_fs_request_app_media_path()` / `pxa_fs_parse_app_media_path()`；C++ SDK 使用 `FilesystemService::app_media_path()`。结果仅用于显示，实际访问仍使用 `app-media-root` 返回的能力句柄。应用应在菜单或初始化时查询，不能在每帧查询。

旧应用可以继续运行；仅使用此接口的组件需要声明 FS 最低版本 0.4 和对应特性。桌面媒体提供者也实现显示查询。

## 商店服务器与认证作者

服务器目录：`/home/lucinhu/Documents/workspace/python/app_store_backend`。数据库迁移 `0007_publisher_namespaces` 增加作者 UUID、唯一 handle、成员关系、不可变应用绑定。绑定只接受商店 application 数字 ID；服务器从已经验证的 PXA 记录获取根密钥，不接受客户端指定签名身份。写接口要求登录、成员权限与 CSRF。公开绑定证明遵守发布、下架及审核状态。

主要接口：

- `GET/POST /api/v3/publishers`
- `POST /api/v3/publishers/{uuid}/applications`
- `GET /api/v3/publishers/{uuid}/bindings/{app_id}`
- `GET /api/v3/publishers/authority`

既有 `/api/v2/apps/{app_id}` 支持 `lineage_root_key_id` 查询参数；同名且没有指定根时返回 409。新商店按完整身份查询、判断安装状态和更新，不选择第一条结果。

认证绑定使用独立签名域、P-256 低 S 签名和有界二进制格式，绑定 issuer、作者 UUID、应用 ID、谱系根、版本及有效期。目录/JSON 证明不能重放为原生认证证明。

生产接入需要：

1. 配置服务器持久 `CATALOG_SIGNING_KEY_PATH`；未配置时认证证明返回不可用。
2. 审核 `/api/v3/publishers/authority` 的公钥，通过 `tools/namespace-issuer.py reviewed-authority.json --output product-issuers.c` 生成仅含公钥的产品策略。
3. 固件构建设置 `PXA_NAMESPACE_ISSUERS_SOURCE` 指向该源文件。设备只信任产品固化的 issuer；网络响应不能添加信任。issuer 换签名密钥时保留稳定 `authority_id`。
4. 按服务器现有部署流程备份数据库后运行 Alembic 升级并部署新服务。

本次只操作临时测试数据库，没有部署服务器、迁移现有数据库或固化未经确认的生产 issuer。默认固件支持离线作者目录；只有产品配置了可信 issuer，才接受认证作者绑定。

## 性能与验证边界

注册、验签、冲突分配、目录迁移和显示路径查询均为安装/激活操作。游戏帧循环不新增目录扫描、字符串格式化、密码运算、文件复制或查询请求。该改动不改变 NES PPU 和 LCD 渲染算法。

实现回归包含注册表 CRC/截断、重名隔离、迁移各阶段失败及不确定提交、真实 POSIX 安装/更新/恢复、私有 FS、C/C++ wire 一致性、服务端到原生真实签名及跨域重放拒绝。设备记录保存在 `local/pxa-identity-implementation-20261021/`，最终实测与状态见其中 `progress.json`；未完成项不会作为通过报告。
