# Legacy Board Adapter Template

[简体中文](README.zh-CN.md)

Copy this pattern beside an existing board implementation. Do not move the
legacy board sources. Replace the `legacy_*` calls in the example with the
product's existing public board, display and control APIs, then call
`pxa_integration_start()` after the existing LVGL display is ready.

The adapter is the only code allowed to include legacy product headers. The
generic PXA components and `pxa-system` remain unaware of the old firmware.
