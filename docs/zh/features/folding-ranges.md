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

<!-- BEGIN CAPABILITY: partial clangd#1661 clangd#2059 -->

**预处理条件折叠**

由 `#else` 分隔的条件分支形成折叠范围

不含 `#else` 的单独 `#if ... #endif` 块尚不支持折叠。

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

<!-- BEGIN CAPABILITY: unsupported -->

**注释折叠**

多行块注释和连续的行注释尚不支持折叠

```snap
tests/snap/folding_range/fold_kinds/08_comment_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**include 区域折叠**

连续的 include 指令尚不形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/09_include_region.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**原始字符串字面量折叠**

多行原始字符串字面量尚不形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/10_raw_string_literal.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`using` 声明块**

连续的 using 声明和 using 指令尚不形成折叠范围

```snap
tests/snap/folding_range/fold_kinds/11_using_declaration_block.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**模板参数列表折叠**

多行模板参数列表尚不形成折叠范围

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

<!-- BEGIN CAPABILITY: unsupported clangd#2666 -->

**声明行折叠**

折叠时尚不能保持声明签名可见

> **客户端支持**：这取决于客户端能否正确解释
> `FoldingRange.startLine`。VS Code 将
> `startLine` 的*下一行*作为隐藏的第一行，因此将 `startLine` 设为
> 声明所在行即可实现预期效果。不过，VS Code 仍会
> 将闭合的 `}` 单独留在一行，而不会将其折叠到
> 签名所在行
> （[vscode#3352](https://github.com/microsoft/vscode/issues/3352) 仍未
> 关闭）。其他客户端的行为可能不同。

```snap
tests/snap/folding_range/refinements/02_fold_from_declaration_line.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**非活动预处理分支标识**

尚不能在视觉上区分非活动分支，也不支持自动折叠这些分支

服务器会为条件与 `#else` 之间的区域生成折叠范围，因此可以手动折叠第一个分支；`#else` 之后的分支目前还没有折叠范围。此处尚未实现识别哪个分支处于*未激活*状态并将其淡化或自动折叠的功能；这类信息由未激活区域功能提供。

> **注意**：这与语义 Token（未激活代码淡化）有所重叠，
> 也部分涉及客户端的用户体验。服务器可以用
> `FoldingRangeKind.Region` 标记这些范围，客户端可以选择自动折叠它们。

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
