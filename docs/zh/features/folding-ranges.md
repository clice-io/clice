# 折叠范围

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

## 折叠类型

<!-- BEGIN GENERATED ITEMS: fold_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**块折叠**

函数、类型、命名空间和 Lambda 形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/01_block_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套复合语句折叠**

嵌套的控制流语句体形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/02_nested_compound_statement.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**多行列表折叠**

跨多行的形参列表、实参列表、初始化器列表和捕获列表形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/03_multiline_list_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**访问说明符区段折叠**

类内由访问说明符划分的区域形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/04_access_specifier_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1661 clangd#2059 -->

**预处理条件折叠**

每个条件分支都形成一个折叠范围，延伸至结束该分支的指令处，该指令本身保持可见

```snap
tests/snap/folding_range/fold_kinds/05_preprocessor_conditional.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**pragma 区域折叠**

命名的 pragma 区域形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/06_pragma_region.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**pragma 分类**

仅根据第一个参数 Token 判断是 region 还是 endregion

```snap
tests/snap/folding_range/fold_kinds/07_pragma_classification.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**注释折叠**

多行块注释和连续的行注释支持折叠

相邻各行上的行注释合为一组，从首行之下开始折叠，首行保持可见；空行或代码行会结束这一组，跟在代码后面的注释不并入任何一组。块注释像一对大括号那样在其定界符处折叠。

```snap
tests/snap/folding_range/fold_kinds/08_comment_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**include 区域折叠**

连续的 include 指令合为一组，从第一条 include 之下开始折叠

空行、注释行或其他指令会结束这一组；条件分支内的 include 在该分支内部折叠，无论该分支是否被选中。

```snap
tests/snap/folding_range/fold_kinds/09_include_region/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**原始字符串字面量折叠**

多行原始字符串字面量在其定界符处折叠

占位文本会重复编码前缀、自定义定界符和字面量后缀。写在宏实参中的原始字符串在其书写位置折叠。

```snap
tests/snap/folding_range/fold_kinds/10_raw_string_literal.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`using` 声明块**

连续的 using 声明和 using 指令从第一条之下开始折叠

空行或其他任何行都会结束这一组，别名声明不会并入其中。由宏生成的 using 声明在宏调用处折叠。

```snap
tests/snap/folding_range/fold_kinds/11_using_declaration_block.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板参数列表折叠**

多行模板参数列表在其尖括号处折叠

类模板、函数模板、变量模板、别名模板、偏特化、类外成员定义中重复书写的模板参数列表、模板模板参数，以及带显式模板参数的 Lambda 均适用。

```snap
tests/snap/folding_range/fold_kinds/12_template_parameter_list.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板特化与实例化**

源码中编写的特化及其成员支持折叠；实例化生成的声明复用模板原型的源码位置，不会重复折叠

```snap
tests/snap/folding_range/fold_kinds/13_template_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**简写函数模板**

带有 `auto` 或受约束的 `auto` 参数的函数，其函数体与其他函数一样支持折叠

```snap
tests/snap/folding_range/fold_kinds/14_abbreviated_function_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏生成内容的折叠**

通过宏生成的大括号和访问说明符在宏调用处形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/15_macro_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**协程体**

源码中编写的块只形成一个折叠范围，协程转换生成的包装层不会增加重复的折叠范围；协程 Lambda 保留其函数体的折叠范围

```snap
tests/snap/folding_range/fold_kinds/16_coroutine_body.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**初始化列表构造**

构造表达式的大括号与嵌套的初始化列表共用定界符，只形成一个折叠范围；列表实参外有圆括号时，则保留两个折叠范围

```snap
tests/snap/folding_range/fold_kinds/17_initializer_list_construction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参折叠**

宏实参中书写的代码在其书写位置折叠

```snap
tests/snap/folding_range/fold_kinds/18_macro_argument_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**链接说明（linkage specification）块**

`extern "C"` 块形成折叠范围，被常见的 `__cplusplus` 守卫包裹时也是如此

```snap
tests/snap/folding_range/fold_kinds/19_linkage_specification.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块片段与 export 块**

全局模块片段、私有模块片段和 `export` 块形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/20_module_blocks.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 改进

<!-- BEGIN GENERATED ITEMS: refinements -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**`collapsedText` 占位文本（LSP 3.17）**

折叠后的范围可显示摘要

> **客户端支持**：VS Code 尚**不支持** `collapsedText`
> （[vscode#70794](https://github.com/microsoft/vscode/issues/70794) 仍未关闭）；
> 使用 nvim-lsp 的 Neovim 原生支持此功能。
> 未实现此字段的客户端会静默忽略它，折叠功能仍然可用，
> 只是不会显示占位文本。

```snap
tests/snap/folding_range/refinements/01_collapsed_text.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2666 -->

**声明行折叠**

声明体从其名称所在行开始折叠

当函数、类或命名空间的左大括号位于名称下方时——无论是独占一行，还是跟在跨越多行的签名之后——按整行折叠的客户端会从名称所在行开始折叠，折叠后的声明仍能看出它是什么。按字符折叠的客户端从大括号处开始折叠，大括号之前的内容本来就保持可见。名称与大括号之间若有条件编译指令，折叠仍从大括号处开始。

> **客户端支持**：VS Code 仍会将闭合的 `}` 单独留在一行，而不会将其折叠到签名所在行
> （[vscode#3352](https://github.com/microsoft/vscode/issues/3352) 仍未关闭）。

```snap
tests/snap/folding_range/refinements/02_fold_from_declaration_line.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**非活动预处理分支**

未选中的分支与选中的分支一样可以折叠

预处理条件的每个分支都可以折叠，无论编译时是否选中该分支，嵌套在未选中分支中的条件也不例外，因此可以手动把不参与编译的代码折叠起来。未选中的代码由语义 Token 的 `inactive` 修饰符淡化显示；折叠范围本身并不区分这些分支。

```snap
tests/snap/folding_range/refinements/03_inactive_preprocessor_branch.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单行结构保持展开**

不隐藏任何内容的折叠只会造成干扰

```snap
tests/snap/folding_range/refinements/04_single_line_constructs.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
