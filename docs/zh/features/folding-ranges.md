# 折叠范围

<!-- The cards in the sections below are generated from the documented cases
     in tests/folding_ranges/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 代码块

命名空间、类型、函数、Lambda 和语句体都可以在大括号处折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/blocks -->

<!-- BEGIN CAPABILITY: supported -->

**大括号代码块**

命名空间、类型、函数和 Lambda 在大括号处折叠

```case
tests/folding_ranges/blocks/01_braces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句体**

控制流语句的语句体和独立代码块可折叠

`if`/`else` 或 `try`/`catch` 的每个分支各自独立折叠：结束上一分支并开始下一分支的那一行保持可见。

```case
tests/folding_ranges/blocks/03_statement_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**链接说明（linkage specification）块**

`extern "C"` 块形成折叠范围，带有常见 `__cplusplus` 条件保护时也不例外

```case
tests/folding_ranges/blocks/04_linkage_specifications
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 列表

任何跨行的圆括号、方括号或大括号都可以折叠，模板参数列表和模板实参列表也是如此。

<!-- BEGIN GENERATED ITEMS: folding_ranges/lists -->

<!-- BEGIN CAPABILITY: supported -->

**多行列表**

多行形参列表、实参列表、初始化列表和捕获列表形成折叠范围

```case
tests/folding_ranges/lists/01_multiline_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板参数列表**

多行模板参数列表在尖括号处折叠

类模板、函数模板、变量模板、别名模板、偏特化、类外成员定义重复书写的参数列表、模板模板参数以及带显式模板参数的 Lambda，其参数列表均可折叠。

```case
tests/folding_ranges/lists/02_template_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**所有括号对**

任何跨行的圆括号、方括号或大括号均可折叠，无论属于哪种语法结构

条件、类型转换、`static_assert`、括号表达式、下标、属性列表、函数类型以及宏调用的实参，都与函数调用的实参一样折叠。写成 `[[...]]` 的属性列表只折叠一次。

```case
tests/folding_ranges/lists/05_every_bracket_pair
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 区段

访问说明符下的成员以及 `case` 标签下的语句，会作为一个区段折叠到下一个标签之前。

<!-- BEGIN GENERATED ITEMS: folding_ranges/sections -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**访问说明符区段**

类内由访问说明符划分的区域形成折叠范围

```case
tests/folding_ranges/sections/01_access_specifiers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**switch 分支**

switch 的每个分支折叠到下一个标签之前，该标签保持可见

连续书写的多个标签合为一个分支折叠。若分支体是从标签所在行开始的代码块，则直接按该代码块折叠。

```case
tests/folding_ranges/sections/02_switch_cases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 注释与连续行组

块注释在定界符处折叠，连续的行注释、包含指令、import 声明或 `using` 声明则合为一组折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/runs -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

多行块注释和连续的行注释可折叠

相邻各行上的行注释合为一组，从首行下方开始折叠，首行保持可见；空行或代码行会结束这一组。块注释与大括号对一样在定界符处折叠。跟在代码后面的行尾注释不折叠。

```case
tests/folding_ranges/runs/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 包含指令组**

连续的 包含指令合为一组，从第一条 include 下方开始折叠

include 之间的空行和注释行仍归入同一组；代码或其他预处理指令会结束这一组。条件分支内的 include 在该分支内部折叠，无论该分支是否启用。

```case
tests/folding_ranges/runs/02_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`using` 声明组**

连续的 using 声明和 using 指令从第一条下方开始折叠

空行或任何其他行都会结束这一组，别名声明不会并入其中，与其他代码共处一行的声明也不会。由宏展开产生的 using 声明在宏调用处折叠。

```case
tests/folding_ranges/runs/03_using_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 import 声明组**

连续的 import 声明与 包含指令一样合为一组折叠

`export import` 也会并入同一组，import 之间的空行和注释行同样留在组内。

```case
tests/folding_ranges/runs/04_import_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 原始字符串

跨行的原始字符串字面量在定界符处折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/raw_strings -->

<!-- BEGIN CAPABILITY: supported -->

**原始字符串字面量**

多行原始字符串字面量在定界符处折叠

占位文本会保留编码前缀、自定义定界符和字面量后缀。写在宏实参中的原始字符串在其书写位置折叠。

```case
tests/folding_ranges/raw_strings/01_raw_string_literals
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理条件

预处理条件的每个分支无论是否启用，都会折叠到结束该分支的指令之前；未启用分支内部的代码块同样可折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/conditionals -->

<!-- BEGIN CAPABILITY: supported clangd#1661 clangd#2059 -->

**预处理条件**

预处理条件的每个分支形成折叠范围，止于结束该分支的指令之前，该指令保持可见

若某个分支会与代码块或区段交叉（例如 C 头文件中 `#ifdef __cplusplus` 包住 `extern "C" {` 的写法），该分支会让位于代码块或区段，保证所有折叠始终保持嵌套关系。

```case
tests/folding_ranges/conditionals/01_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未启用的分支**

未启用的分支与启用的分支一样可折叠

预处理条件的每个分支无论编译时是否启用均可折叠，嵌套在未启用分支中的条件也不例外，便于手动收起不参与编译的代码。未启用的代码由语义 Token 的 `inactive` 修饰符变暗显示；折叠范围本身不区分分支是否启用。

```case
tests/folding_ranges/conditionals/02_untaken_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未启用分支中的代码块**

未启用分支内部的代码块同样可折叠，而不只是整个分支

编译跳过的代码中，函数、类或语句体仍可在括号处折叠，便于逐块收起不参与编译的代码。

```case
tests/folding_ranges/conditionals/05_blocks_in_untaken_branches
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## pragma 区域

`#pragma region` 和 `#pragma endregion` 折叠两者之间的代码。

<!-- BEGIN GENERATED ITEMS: folding_ranges/regions -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**pragma 区域**

具名 pragma 区域形成折叠范围

```case
tests/folding_ranges/regions/01_pragma_regions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块单元中的模块片段与 `export` 块可折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块片段与 export 块**

全局模块片段、私有模块片段与 `export` 块形成折叠范围

```case
tests/folding_ranges/modules/01_module_blocks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

通过宏展开拼出的代码，或写在宏实参中的代码，都在源码可见的位置折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/macros -->

<!-- BEGIN CAPABILITY: supported -->

**由宏拼出的代码块**

通过宏展开拼出的大括号与访问说明符在宏调用处折叠

由两次宏调用分别开启与闭合的代码块在这两处调用之间折叠，两处调用保持可见，占位文本为 `...`。

```case
tests/folding_ranges/macros/01_macro_spelled_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参中的代码**

写在宏实参内部的代码在其书写位置折叠

```case
tests/folding_ranges/macros/02_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 按整行折叠

按整行折叠的客户端会把辨识声明所需的内容留在屏幕上：声明体从名称所在行开始折叠，支持显示折叠文本的客户端还会把结尾行也折叠进去。

<!-- BEGIN GENERATED ITEMS: folding_ranges/line_folding -->

<!-- BEGIN CAPABILITY: supported clangd#2666 -->

**声明行折叠**

声明体从声明名称所在行开始折叠

当左大括号位于名称下方——无论独占一行还是跟在跨越多行的签名之后——按整行折叠的客户端都会从名称所在行开始折叠，使折叠后的声明依然一目了然。按字符折叠的客户端则从左大括号处开始折叠，大括号之前的内容本就保持可见。

> **客户端支持**：VS Code 会将右大括号 `}` 单独留在自身所在行，而不会合并到签名行（[vscode#3352](https://github.com/microsoft/vscode/issues/3352)）；Neovim 则会将其合并（见“连同结尾行折叠”）。

```case
tests/folding_ranges/line_folding/01_declaration_lines
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连同结尾行折叠**

支持显示折叠文本的客户端会把代码块的结尾行也折叠起来，显示为 `int area(Point p) {...}`

每处折叠显示首行后接 `...`；当右大括号、`#endif` 或 `*/` 独占一行时，该行也会折叠，显示在 `...` 之后。同时开启下一代码块的结尾行（如 `} else {`）则保持可见。

> **客户端支持**：Neovim 通过 `vim.lsp.foldtext()` 显示折叠文本。VS Code 不显示折叠文本，因此结尾行在 VS Code 中保持可见（[vscode#3352](https://github.com/microsoft/vscode/issues/3352)）。

```case
tests/folding_ranges/line_folding/07_collapsed_lines
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 呈现

折叠后的范围以占位文本表示隐藏的内容；不会隐藏任何内容的折叠则不予提供。

<!-- BEGIN GENERATED ITEMS: folding_ranges/presentation -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**折叠文本**

折叠后的范围显示占位文本（如函数体显示 `{...}`），起始行本身保持可见

按字符折叠的客户端（如 Zed）在折叠范围处直接显示占位文本。按整行折叠的客户端（如 Neovim）会用折叠文本替换整行首行，因此收到的是首行后接 `...` 的文本（见“连同结尾行折叠”）。

> **客户端支持**：VS Code 不显示 `collapsedText`（[vscode#70794](https://github.com/microsoft/vscode/issues/70794)）。忽略该字段的客户端仍可正常折叠，只是不显示占位文本。

```case
tests/folding_ranges/presentation/01_collapsed_text
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单行结构保持展开**

不隐藏任何内容的折叠只会造成干扰

```case
tests/folding_ranges/presentation/02_single_line_constructs
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
