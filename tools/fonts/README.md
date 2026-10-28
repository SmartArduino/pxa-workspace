# 按设备生成系统字体

默认工厂字体使用 `zh-Hans,en` 加学习音标语料：2013《通用规范汉字表》全部 **8105** 字、英文 ASCII、拼音声调、常用英语 IPA，以及原系统的标点、数学符号、箭头和图标映射。简体字形使用 **Noto Sans CJK SC** 大陆黑体。文件仍叫 `noto_sans_cjk_common.ttf`，内部名称为 PXA Sans SC，当前为 **2,787,448 B**。拼音和音标语料位于 `study-phonetics.txt`，不能用普通 ASCII 字体代替，否则会丢失声调或音素。

字体和拼音字表独立裁剪。只编译简体输入法并不会自动删除 TTF 内的繁体轮廓；选择精简字体也不会自动扩大 Provider 的字表。8105 字是规范汉字表覆盖，不等于 Unicode 中所有汉字，更不等于智能词句输入。

## 一份字体，构建时选择字集

构建端需要 Python `fontTools` 和 OFL 授权的 Noto Sans CJK Regular TTC，设备不需要 Python，也不必保存源 TTC。选择多个字集后，生成器合并为**一份 TTF**；未选字集的映射及无引用轮廓会删除，复合字形所需部件保留。无需新增运行时语言 fallback 链、字体图集或整份字体内存副本。各配置还保留从实际系统资源目录和键盘标签源码收集的 267 个固定 UI 字符，避免只选繁体、日文或韩文后，现有菜单和按键标签缺字；这是必要文案，不是一整套额外中文词库。

从项目根目录运行：

```sh
font_output="local/artifacts/fonts/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$font_output"
git show 51e308e:factory/base/system/fonts/noto_sans_cjk_common.ttf > "$font_output/base.ttf"
python3 tools/fonts/build_common_font.py \
  --base "$font_output/base.ttf" \
  --source /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc \
  --languages zh-Hans,en \
  --corpus tools/fonts/study-phonetics.txt \
  --fallback-source /usr/share/fonts/noto/NotoSans-Regular.ttf \
  --output "$font_output/noto_sans_cjk_common.ttf" --report "$font_output/build.json"
```

学习音标的补充源为 OFL 1.1 授权的 Noto Sans Regular 2.015。其实际 TTF SHA-256 为
`478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823`；分发时保留工厂字体目录的
`OFL.txt`、`OFL-Latin.txt` 和 `NOTICE`。构建器展开补充 TTF 的复合轮廓后合入主字体，设备仍只加载一份 TTF。
更新既有设备且需保留全部原字形时，使用该设备的字体作 base，省略 `--languages` 并选择
`--traditional none`，仅加入所选 corpus，不隐式扩充或裁剪汉字。先核对旧码位、字宽和渲染结果，再按产品维护流程替换。

| `--languages` | 字集与地区字形 |
|---|---|
| `zh-Hans,en` | 8105 规范汉字、英文；SC 字形，默认设备配置 |
| `zh-Hant,en` | 8833 繁体/Big5 一级常用汉字、英文；TC 字形 |
| `zh-Hans,zh-Hant,en` | 两套中文字符集合、英文；共同码位用 SC 字形 |
| `ja,en` | JIS X 0208/0201、英文；JP 字形 |
| `ko,en` | 现代 Hangul、Jamo、英文；KR 字形，不默认加入汉字 |
| `latin` / `greek` / `cyrillic` | 所选 Noto 源中相应范围的字形及组合附加符；报告列出源字体缺少的范围码位 |

还可以组合以上字集，重复 `--corpus book.txt` 补充应用实际需要的 UTF-8 字符。中文承诺字集和 corpus 缺字会让构建失败，不能悄悄丢字。空白 corpus 字符不扩充覆盖。`en` 只请求英文 ASCII；需要法语等拉丁扩展时选择 `latin`。仅英文设备应同时把系统界面语言设为英文。

地区优先级为 SC、TC、JP、KR，自动选择 TTC face 2、3、0、1；显式 `--face` 会验证地区名称。一个 Unicode 码位在同一份字体中只能对应一种地区字形；中日混排并按段落切换地区字形需要分别选择 face，缺字 fallback 无法解决相同码位的地区差异。此工具没有引入运行时语言包安装或复杂文字排版引擎。

繁体字集有三个罕见汉字不在 Noto CJK 中，使用构建端的 OFL 补充轮廓：

```sh
python3 tools/fonts/build_common_font.py \
  --base /tmp/pxa-font-base.ttf \
  --source /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc \
  --languages zh-Hant,en \
  --fallback-source tools/fonts/fallback/unifont-upper-three.otf \
  --output /tmp/pxa-tc.ttf --report /tmp/pxa-tc.json
```

补充字体只在构建端使用，最终仍是一份 TTF。三个字是 `𡑍𣈶𥗽`，来源及 OFL 授权见 `fallback/`。其轮廓来自 Unifont，风格与 Noto 不完全一致；简体默认不使用它。分发可选繁体构建时须包含补充字体的授权和说明。

不传 `--languages` 时保留原先的扩充模式：不移除 base 的覆盖，再加入 Big5 一级字集和 corpus；`--traditional all` 则请求更大的 Big5 字集。这是向后兼容的生成模式，当前工厂精简字体的复现应显式传 `--languages zh-Hans,en`。

## 验证、内存与安装

```sh
python3 -m unittest discover -s tools/fonts -p 'test_*.py'
python3 deps/pxa-system/tools/ime/generate_builtin.py \
  --check --corpus simplified --corpus-output /tmp/pxa-sc.txt
cc tools/fonts/ft_font_probe.c -I/usr/include/freetype2 -lfreetype \
  -Wall -Wextra -Werror -o /tmp/pxa-font-probe
/tmp/pxa-font-probe /tmp/noto_sans_cjk_common.ttf /tmp/pxa-sc.txt
PXA_FONT_PROBE=/tmp/pxa-font-probe python3 -m unittest discover -s tools/fonts -p 'test_*.py'
```

原生 probe 使用产品相同 FreeType 加载标记，逐字检查 12/16/28/56 px 的实际轮廓、缺字、空白及分配。`compare BEFORE AFTER UTF8_TEXT` 比较保留字符的像素、字宽与基线。补充平面字使用标准 Unicode cmap，不改变 Host 文本 ABI。输出始终是 TrueType 轮廓；不使用启动开销较高的 CFF 运行时 face。生成器共享完全相同的轮廓和度量，压缩 BMP cmap，并保留可见的 `.notdef`。有补充平面字时只保留一个 format 12 cmap，其中也包含 BMP 映射；BMP-only 字体仍使用紧凑 format 4。[OpenType 规范](https://learn.microsoft.com/typography/opentype/spec/cmap#format-12-segmented-coverage)允许不携带旧式 format 4 副本。PXA 的 FreeType 与生成标签所用 Pillow 均验证通过；这不是为只认识 format 4 的旧工具生成的兼容字体。

TTF 缩小主要节省 Flash，不能按文件大小推算 RAM。使用同一字符样本的原生 FreeType 请求字节测试，保留唯一完整的 Unicode cmap 后，打开及渲染峰值比上一轮各减少 **88 B**，与扩字表前的单 charmap 元数据占用相同，关闭后都归零；不包含分配器开销、LVGL 缓存及整机堆。现有的图标/位图字体 fallback 保留，本改动没有新增逐语言查找链。普通命中字形无需遍历其他语言字体；若未来动态安装多字体，每个额外 face 的状态、缺字查询和缓存都需要另行实测。

安装时用选定 TTF 替换设备的 `system/fonts/noto_sans_cjk_common.ttf`，并保留 `NOTICE`、`OFL.txt`。**文件替换须在所有旧 face 已关闭时进行**。当前 LittleFS VFS 禁止覆盖打开的目标，原生系统 UI 与 PXA Host UI 都可能持有字体；仅停止前台应用不足以释放。维护启动中禁用两处加载，再原子上传、核对摘要，最后恢复正常固件。不要擦除分区、用户数据或 Wi-Fi 来换字体，也不要在运行中的 face 背后直接截断写入。

生成器先写临时文件，完整校验后原子替换输出；失败保留原文件，并保留既有输出权限（新输出继承 base 权限）。支持 base 原位更新并正确记录原始摘要，拒绝用输出覆盖源字体或 corpus，TTF 读取句柄会在替换前关闭。切换地区时会从对应源 face 更新 CJK 字形，不能将 TC/JP base 的字形仅改名为 SC；仅同地区才保留原轮廓。字形去重不会把有效字符合并为 `.notdef`（glyph ID 0）。

进一步完善的验证见 `deps/pxa-system/docs/zh-CN/input-methods-refinement-20261015.md`；首次扩字表的记录见 `deps/pxa-system/docs/zh-CN/input-methods-character-coverage-20261015.md`。之前阅读器报告的繁体语料验收适用于此前的较大字体；默认精简配置明确不再包含未选语言，阅读繁体作品应选择双字集或相应 corpus。
