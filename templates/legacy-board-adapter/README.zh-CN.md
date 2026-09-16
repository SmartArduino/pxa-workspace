# 旧板级 Adapter 模板

[English](README.md)

将此模式复制到已有板子实现的相邻目录，不要迁移旧板级源码。把示例中的 `legacy_*`
调用替换为产品已有的公开 Board、显示和控制 API，然后在既有 LVGL display 准备好后
调用 `pxa_integration_start()`。

adapter 是唯一允许包含旧产品头文件的代码；通用 PXA component 与 `pxa-system` 不会
知道旧固件的任何类型或目录结构。
