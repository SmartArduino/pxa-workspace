# 701 RPC 协议

主机与 AC701N 之间的 RPC 消息定义，以及由它生成的 protobuf-c 代码。

**这个仓是协议的唯一出处。** 701 固件的每个分支、以及主机端驱动，都以
submodule 的形式引用它——所以"哪个分支支持什么功能"和"消息编号是多少"是两件
互不相干的事：分支之间只可能指向不同的 commit，不可能各自演化出不同的编号。

```
rpc_messages.proto        消息定义，唯一需要手改的文件
rpc_messages.pb-c.c/.h    生成物，入库
tools/generate.sh         重新生成（protobuf-c 版本钉死在 1.3.3）
tools/check_ids.py        编号追加性检查
```

## 编号规则

**只许在末尾追加。已经存在的号，值不许改、名字不许换、删掉也不许被别人复用。**

编号是对外协议的一部分，它跟着固件出厂。改一个号，此前烧出去的设备和之后编译
的主机驱动就对不上话了——而且不会报错，只是把 A 消息当成 B 消息执行。

这不是假设：`7374216 fix: break 修改rpc编号，兼容适配`（2026-07-16）把 297~305
和 553~561 整体挪了一位，`Req_ButtonConfig` 从 298 变成 297，`Req_GpioConfig`
从 299 变成 298，一路顺延。停在那次改动之前的固件（例如 cxx_tflm 分支）与之后
的主机驱动之间，配 GPIO 会被执行成配按键。`tools/check_ids.py` 就是为了让这种
改动在落库之前失败，而不是在真机上表现成灵异现象。

另外三条同时检查：

- `Resp_X == Req_X + 256`
- oneof 字段号 == 对应的 RpcId
- `Req_Max` / `Resp_Max` / `Evt_Max` == 同族最大值 + 1（哨兵本来就该随追加移动）

要废弃一个消息：**保留枚举条目**（可以加注释说明已废弃），或者在 proto 里写
`reserved`。直接删掉会让那个号被下一个人捡去用。

## 改协议的流程

```sh
vim rpc_messages.proto      # 只在末尾追加
tools/generate.sh           # 重新生成 pb-c
tools/check_ids.py          # 编号检查（pre-commit 也会跑）
git commit -a
```

生成物入库而不是各人本地生成：protobuf-c 换个版本产出就不一样，会在仓库里表现
为一大片无意义 diff，也让"固件和主机用的是不是同一份"变得无法确认。
`generate.sh` 因此会检查版本，不是 1.3.3 直接拒绝。

装钩子（每个 clone 各装一次）：

```sh
git config core.hooksPath tools/hooks
```

## 在固件仓里用

作为 submodule 挂在 `apps/coprocessor/rpc/protobuf/`。更新协议：

```sh
git -C apps/coprocessor/rpc/protobuf pull
git add apps/coprocessor/rpc/protobuf     # 记录新的指向
```
