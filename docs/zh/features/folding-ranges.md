# 折叠范围

<!-- The cards in the sections below are generated from the documented cases
     in tests/folding_ranges/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 代码块

命名空间、类型、函数、Lambda 和语句体都可以在大括号处折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/blocks -->

<!-- BEGIN CAPABILITY: supported -->

**大括号代码块**

命名空间、类型、函数和 Lambda 可在大括号处折叠

```case
tests/folding_ranges/blocks/01_braces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句体**

控制流语句的语句体和独立代码块支持折叠

`if`/`else` 或 `try`/`catch` 的每个分支都可以单独折叠：结束一个分支并开始下一个分支的那一行保持可见。

```case
tests/folding_ranges/blocks/03_statement_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**链接说明（linkage specification）块**

`extern "C"` 块会形成折叠范围，受常见的 `__cplusplus` 条件编译保护时也不例外

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

多行形参列表、实参列表、初始化列表和捕获列表会形成折叠范围

```case
tests/folding_ranges/lists/01_multiline_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板参数列表**

多行模板参数列表在其尖括号处折叠

类模板、函数模板、变量模板、别名模板、偏特化、模板模板参数和带显式模板参数的 Lambda 的参数列表都可以折叠，类外成员定义中重复书写的参数列表也是如此。

```case
tests/folding_ranges/lists/02_template_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**所有括号对**

任何跨行的圆括号、方括号或大括号都可以折叠，无论它们属于哪种语法结构

条件、类型转换、`static_assert`、括号表达式、下标、属性列表、函数类型以及宏调用的实参，都像函数调用的实参一样折叠。写成 `[[...]]` 的属性列表只折叠一次。

```case
tests/folding_ranges/lists/05_every_bracket_pair
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 区段

访问说明符下的成员和 `case` 标签下的语句，都会作为一个区段折叠到下一个标签之前。

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

switch 的每个分支都折叠到下一个标签之前，该标签保持可见

接连书写的多个标签合为一个分支折叠。分支体若是从标签所在行开始的代码块，该分支就按这个代码块折叠。

```case
tests/folding_ranges/sections/02_switch_cases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 注释与连续行组

块注释在其定界符处折叠，连续的行注释、include 指令、import 声明或 `using` 声明则合为一组折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/runs -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

多行块注释和连续的行注释支持折叠

相邻各行上的行注释合为一组，从首行之下开始折叠，首行保持可见；空行或代码行会结束这一组。块注释像一对大括号那样在其定界符处折叠。跟在代码后面的注释不折叠。

```case
tests/folding_ranges/runs/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 include 指令组**

连续的 include 指令合为一组，从第一条 include 之下开始折叠

include 之间的空行和注释行仍属于这一组，代码或其他指令会结束这一组。条件分支内的 include 在该分支内部折叠，无论该分支是否被选中。

```case
tests/folding_ranges/runs/02_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 `using` 声明组**

连续的 using 声明和 using 指令从第一条之下开始折叠

空行或其他任何行都会结束这一组，别名声明不会并入其中，与其他代码同处一行的声明也不会。由宏生成的 using 声明在宏调用处折叠。

```case
tests/folding_ranges/runs/03_using_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 import 声明组**

连续的 import 声明像 include 指令一样合为一组折叠

`export import` 也会并入这一组，import 之间的空行和注释行同样留在组内。

```case
tests/folding_ranges/runs/04_import_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 原始字符串

跨行的原始字符串字面量在其定界符处折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/raw_strings -->

<!-- BEGIN CAPABILITY: supported -->

**原始字符串字面量**

多行原始字符串字面量在其定界符处折叠

占位文本会重复编码前缀、自定义定界符和字面量后缀。写在宏实参中的原始字符串在其书写位置折叠。

```case
tests/folding_ranges/raw_strings/01_raw_string_literals
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理条件

预处理条件的每个分支都可以折叠到结束该分支的指令之前，无论该分支是否被选中；未选中分支中的代码块同样可以折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/conditionals -->

<!-- BEGIN CAPABILITY: supported clangd#1661 clangd#2059 -->

**预处理条件**

预处理条件的每个分支都会形成折叠范围，止于结束该分支的指令之前，该指令保持可见

若某个分支会与代码块或区段交叉，例如 C 头文件中 `#ifdef __cplusplus` 包在 `extern "C" {` 外围的情形，该分支就让位于代码块或区段，因此折叠总能保持嵌套。

```case
tests/folding_ranges/conditionals/01_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未选中的分支**

未选中的分支与选中的分支一样可以折叠

预处理条件的每个分支都可以折叠，无论编译时是否选中该分支，嵌套在未选中分支中的条件也不例外，因此可以手动把不参与编译的代码折叠起来。未选中的代码由语义 Token 的 `inactive` 修饰符淡化显示；折叠范围本身并不区分这些分支。

```case
tests/folding_ranges/conditionals/02_untaken_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未选中分支中的代码块**

未选中分支中的代码块同样可以折叠，而不只是整个分支

编译时跳过的代码中，函数、类或语句体都在其括号处折叠，因此可以把不参与编译的代码逐块折叠起来。

```case
tests/folding_ranges/conditionals/05_blocks_in_untaken_branches
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## pragma 区域

`#pragma region` 和 `#pragma endregion` 之间的代码可以折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/regions -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**pragma 区域**

命名的 pragma 区域形成折叠范围

```case
tests/folding_ranges/regions/01_pragma_regions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块单元中的模块片段和 `export` 块可以折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块片段与 export 块**

全局模块片段、私有模块片段和 `export` 块会形成折叠范围

```case
tests/folding_ranges/modules/01_module_blocks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

由宏生成的代码或写在宏实参中的代码，都可以在其显示位置折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/macros -->

<!-- BEGIN CAPABILITY: supported -->

**由宏生成的代码块**

由宏生成的大括号和访问说明符在宏调用处折叠

由宏调用打开、又由宏调用关闭的代码块，在这两处调用之间折叠，两处调用都保持可见；其占位符为 `...`。

```case
tests/folding_ranges/macros/01_macro_spelled_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参中的代码**

宏实参中书写的代码在其书写位置折叠

```case
tests/folding_ranges/macros/02_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 按整行折叠

按整行折叠的客户端会把读者需要的内容留在屏幕上：声明体从声明名称所在行开始折叠；显示折叠文本的客户端还会把结尾行一并折叠进去。

<!-- BEGIN GENERATED ITEMS: folding_ranges/line_folding -->

<!-- BEGIN CAPABILITY: supported clangd#2666 -->

**声明行折叠**

声明体从其名称所在行开始折叠

当左大括号位于名称下方，独占一行或跟在跨越多行的签名之后时，按整行折叠的客户端会从名称所在行开始折叠，因此折叠后仍能辨认出该声明。按字符折叠的客户端则从左大括号处开始折叠，其前面的所有内容始终保持可见。

> **客户端支持**：VS Code 会将右大括号 `}` 单独留在一行，
> 而不会将其合并到签名所在行
> （[vscode#3352](https://github.com/microsoft/vscode/issues/3352)）；
> Neovim 则会合并（见“结尾行一并折叠”）。

```case
tests/folding_ranges/line_folding/01_declaration_lines
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结尾行一并折叠**

显示折叠文本的客户端也会折叠代码块的结尾行，显示为 `int area(Point p) {...}`

每个折叠显示其首行，后接 `...`；当右大括号、`#endif` 或 `*/` 独占一行时，该行会一并折叠，并显示在 `...` 之后。同时开始下一个代码块的结尾行（例如 `} else {`）保持可见。

> **客户端支持**：Neovim 通过 `vim.lsp.foldtext()` 显示该文本。
> VS Code 不显示折叠文本，因此在 VS Code 中结尾行保持可见
> （[vscode#3352](https://github.com/microsoft/vscode/issues/3352)）。

```case
tests/folding_ranges/line_folding/07_collapsed_lines
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 呈现

折叠后的范围会用占位符表示隐藏的内容；没有内容可隐藏时，则不提供折叠。

<!-- BEGIN GENERATED ITEMS: folding_ranges/presentation -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**折叠文本**

折叠后的范围会显示占位符，例如用 `{...}` 表示声明体，折叠范围的起始行仍保持可见

按字符折叠的客户端（例如 Zed）用占位符替换折叠范围。按整行折叠的客户端（例如 Neovim）用占位符替换整个首行，因此改为显示首行后接 `...`（见“结尾行一并折叠”）。

> **客户端支持**：VS Code 尚不显示 `collapsedText`
> （[vscode#70794](https://github.com/microsoft/vscode/issues/70794)）。
> 忽略它的客户端仍能折叠，但不会显示占位符。

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
