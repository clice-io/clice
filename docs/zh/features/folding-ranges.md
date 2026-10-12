# 折叠范围

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/folding_range/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 代码块

命名空间、类型、函数、Lambda 和语句体都可以在大括号处折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/blocks -->

<!-- BEGIN CAPABILITY: supported -->

**大括号代码块**

命名空间、类型、函数和 Lambda 可在大括号处折叠

```case
tests-next/folding_range/blocks/01_braces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句体**

控制流语句的语句体和独立代码块支持折叠

`if`/`else` 或 `try`/`catch` 的每个分支都可以单独折叠：结束一个分支并开始下一个分支的那一行保持可见。

```case
tests-next/folding_range/blocks/03_statement_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**链接说明（linkage specification）块**

`extern "C"` 块会形成折叠范围，受常见的 `__cplusplus` 条件编译保护时也不例外

```case
tests-next/folding_range/blocks/04_linkage_specifications
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 列表

跨行的形参列表、实参列表、初始化列表、捕获列表和模板参数列表都可以折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/lists -->

<!-- BEGIN CAPABILITY: supported -->

**多行列表**

多行形参列表、实参列表、初始化列表和捕获列表会形成折叠范围

```case
tests-next/folding_range/lists/01_multiline_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板参数列表**

多行模板参数列表在其尖括号处折叠

类模板、函数模板、变量模板、别名模板、偏特化、模板模板参数和带显式模板参数的 Lambda 的参数列表都可以折叠，类外成员定义中重复书写的参数列表也是如此。

```case
tests-next/folding_range/lists/02_template_parameters
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 访问控制区段

访问说明符下的成员可以作为类中的一个区段整体折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/sections -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**访问说明符区段**

类内由访问说明符划分的区域形成折叠范围

```case
tests-next/folding_range/sections/01_access_specifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 注释与连续行组

块注释在其定界符处折叠，连续的行注释、include 指令或 `using` 声明则合为一组折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/runs -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

多行块注释和连续的行注释支持折叠

相邻各行上的行注释合为一组，从首行之下开始折叠，首行保持可见；空行或代码行会结束这一组。块注释像一对大括号那样在其定界符处折叠。跟在代码后面的注释不折叠。

```case
tests-next/folding_range/runs/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 include 指令组**

连续的 include 指令合为一组，从第一条 include 之下开始折叠

空行、注释行或其他指令会结束这一组；条件分支内的 include 在该分支内部折叠，无论该分支是否被选中。

```case
tests-next/folding_range/runs/02_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**连续 `using` 声明组**

连续的 using 声明和 using 指令从第一条之下开始折叠

空行或其他任何行都会结束这一组，别名声明不会并入其中，与其他代码同处一行的声明也不会。由宏生成的 using 声明在宏调用处折叠。

```case
tests-next/folding_range/runs/03_using_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 原始字符串

跨行的原始字符串字面量在其定界符处折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/raw_strings -->

<!-- BEGIN CAPABILITY: supported -->

**原始字符串字面量**

多行原始字符串字面量在其定界符处折叠

占位文本会重复编码前缀、自定义定界符和字面量后缀。写在宏实参中的原始字符串在其书写位置折叠。

```case
tests-next/folding_range/raw_strings/01_raw_string_literals
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理条件

预处理条件的每个分支都可以折叠到结束该分支的指令之前，无论该分支是否被选中。

<!-- BEGIN GENERATED ITEMS: folding_range/conditionals -->

<!-- BEGIN CAPABILITY: supported clangd#1661 clangd#2059 -->

**预处理条件**

预处理条件的每个分支都会形成折叠范围，止于结束该分支的指令之前，该指令保持可见

```case
tests-next/folding_range/conditionals/01_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未选中的分支**

未选中的分支与选中的分支一样可以折叠

预处理条件的每个分支都可以折叠，无论编译时是否选中该分支，嵌套在未选中分支中的条件也不例外，因此可以手动把不参与编译的代码折叠起来。未选中的代码由语义 Token 的 `inactive` 修饰符淡化显示；折叠范围本身并不区分这些分支。

```case
tests-next/folding_range/conditionals/02_untaken_branches
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## pragma 区域

`#pragma region` 和 `#pragma endregion` 之间的代码可以折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/regions -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**pragma 区域**

命名的 pragma 区域形成折叠范围

```case
tests-next/folding_range/regions/01_pragma_regions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块单元中的模块片段和 `export` 块可以折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块片段与 export 块**

全局模块片段、私有模块片段和 `export` 块会形成折叠范围

```case
tests-next/folding_range/modules/01_module_blocks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

由宏生成的代码或写在宏实参中的代码，都可以在其显示位置折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/macros -->

<!-- BEGIN CAPABILITY: supported -->

**由宏生成的代码块**

由宏生成的大括号和访问说明符在宏调用处折叠

```case
tests-next/folding_range/macros/01_macro_spelled_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参中的代码**

宏实参中书写的代码在其书写位置折叠

```case
tests-next/folding_range/macros/02_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 声明行

声明体从声明名称所在行开始折叠，因此折叠后仍能辨认出该声明。

<!-- BEGIN GENERATED ITEMS: folding_range/line_folding -->

<!-- BEGIN CAPABILITY: supported clangd#2666 -->

**声明行折叠**

声明体从其名称所在行开始折叠

当左大括号位于名称下方，独占一行或跟在跨越多行的签名之后时，按整行折叠的客户端会从名称所在行开始折叠，因此折叠后仍能辨认出该声明。按字符折叠的客户端则从左大括号处开始折叠，其前面的所有内容始终保持可见。

> **客户端支持**：VS Code 仍会将右大括号 `}` 单独留在一行，
> 而不会将其合并到签名所在行
> （[vscode#3352](https://github.com/microsoft/vscode/issues/3352)）。

```case
tests-next/folding_range/line_folding/01_declaration_lines
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 呈现

折叠后的范围会用占位符表示隐藏的内容；没有内容可隐藏时，则不提供折叠。

<!-- BEGIN GENERATED ITEMS: folding_range/presentation -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**折叠文本**

折叠后的范围会显示占位符，例如用 `{...}` 表示声明体，折叠范围的起始行仍保持可见

> **客户端支持**：VS Code 尚不显示 `collapsedText`
> （[vscode#70794](https://github.com/microsoft/vscode/issues/70794)）；
> Neovim 则支持显示。忽略它的客户端仍能折叠，
> 但不会显示占位符。

```case
tests-next/folding_range/presentation/01_collapsed_text
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单行结构保持展开**

不隐藏任何内容的折叠只会造成干扰

```case
tests-next/folding_range/presentation/02_single_line_constructs
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
