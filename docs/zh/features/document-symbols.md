# 文档符号

<!-- The cards in the sections below are generated from the documented cases
     in tests/document_symbols/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

通过 `textDocument/documentSymbol` 提供文件大纲和面包屑导航：返回包含符号范围、选择范围以及 `detail` 字段的嵌套符号树，用来区分重载并展示所声明的类型。

## 符号层级

符号在大纲中的嵌套关系与代码中一致：成员位于所属类之下，局部符号位于所属函数之下。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套作用域**

命名空间、类及其成员在大纲中的嵌套关系与代码中一致

```case
tests/document_symbols/symbol_hierarchy/01_nested_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域外定义（out-of-line definitions）**

作用域外定义在其书写位置以限定名列出，重复打开的命名空间每次打开各列出一次

```case
tests/document_symbols/symbol_hierarchy/02_qualified_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**匿名作用域与内联作用域**

匿名命名空间、匿名结构体和匿名联合体将成员归在占位名称下，内联命名空间保留自身节点

```case
tests/document_symbols/symbol_hierarchy/03_anonymous_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**局部符号**

在函数体中声明的变量、类型和 Lambda 嵌套在该函数之下

```case
tests/document_symbols/symbol_hierarchy/04_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**友元函数定义**

在类内部定义的友元函数列在该类之下

```case
tests/document_symbols/symbol_hierarchy/05_friend_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**访问控制区段**

成员按代码中的书写顺序列出；所属的 `public` 和 `private` 区段不单独构成大纲层级

```case
tests/document_symbols/symbol_hierarchy/06_access_sections
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#500 -->

**宏声明的成员**

单次宏调用以代码中未写出的名称声明的实体会列在该次调用之下，并带上实参中给出的名称（例如测试宏生成的测试、类宏添加的成员）；只声明实参中所写名称的宏调用则直接按所写名称列出

```case
tests/document_symbols/symbol_hierarchy/09_macro_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号种类

每个符号列出时都会标明其种类，涵盖从命名空间、类到宏和模块声明的各类实体。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**核心符号种类**

命名空间、类、联合体、枚举及其成员，以及函数、变量、字段、结构化绑定（structured bindings）和 Lambda 各自按其种类列出

```case
tests/document_symbols/symbol_kinds/01_core_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

类模板、函数模板和变量模板在详情中标明 `template`，其范围从 `template` 关键字开始；概念（concept）和简写函数模板也会列出

```case
tests/document_symbols/symbol_kinds/02_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板特化与推导指引（deduction guides）**

模板特化连同其模板实参和自身成员一起列出，推导指引则带上它推导出的签名

```case
tests/document_symbols/symbol_kinds/03_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

`typedef` 与 `using` 别名以及别名模板均作为类型别名列出

```case
tests/document_symbols/symbol_kinds/04_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**显式实例化**

类模板、函数模板和变量模板的显式实例化会列出，但不展开成员

```case
tests/document_symbols/symbol_kinds/05_explicit_instantiations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**宏**

对象式宏和函数式宏均会列出，函数式宏还会将形参作为详情显示

```case
tests/document_symbols/symbol_kinds/06_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块声明**

模块单元会列出其声明或归属的模块

```case
tests/document_symbols/symbol_kinds/07_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**包含指令**

`#include` 指令不列入大纲；其文档链接可直接跳转到目标文件

```case
tests/document_symbols/symbol_kinds/10_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pragma mark 分组**

`#pragma mark - Name` 开启一个分组，将随后的符号归入其下

```case
tests/document_symbols/symbol_kinds/11_pragma_marks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间别名**

命名空间别名作为命名空间列出，详情显示为 `namespace alias`

```case
tests/document_symbols/symbol_kinds/20_namespace_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件保护宏（include guards）**

包裹整个头文件的 `#ifndef` 与 `#define` 头文件保护宏不列入大纲

```case
tests/document_symbols/symbol_kinds/21_include_guards
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号详情

名称旁的 `detail` 用于区分重载，并展示变量所声明的类型。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**函数签名**

`detail` 字段显示函数的形参（含形参名）和返回类型，用以区分重载；构造函数不显示返回类型

```case
tests/document_symbols/symbol_detail/01_detail_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量与字段类型**

变量或字段将所声明的类型显示为详情，Lambda 则显示调用它时的形参与返回类型

```case
tests/document_symbols/symbol_detail/02_variable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**默认实参**

函数的详情会列出各形参及其默认值

```case
tests/document_symbols/symbol_detail/03_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#601 -->

**基类**

派生类的详情按代码写法列出各个基类及其访问说明符

```case
tests/document_symbols/symbol_detail/04_base_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带类作用域的类型**

书写时带有类作用域的类型在详情中保留一次该作用域，依赖名也不例外

```case
tests/document_symbols/symbol_detail/05_scoped_types
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号范围

每个符号都带有完整声明的范围以及名称本身的范围，选中符号时编辑器会定位并选中名称范围。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**名称与声明范围**

符号的范围覆盖整条声明，名称范围覆盖完整写出的名称，包括 `~Widget`、`operator==` 和 `operator bool`

```case
tests/document_symbols/symbol_ranges/01_selection_ranges
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**多行签名**

跨越多行的声明范围从首行延伸至末行

```case
tests/document_symbols/symbol_ranges/02_multiline_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**宏展开产生的符号**

宏调用所声明的符号列在宏调用处，而非宏定义处

```case
tests/document_symbols/symbol_ranges/03_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**宏实参中的名称**

写在宏实参中的名称作为符号的名称范围；写在宏体中的名称则回退到宏调用处

```case
tests/document_symbols/symbol_ranges/04_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号标签

标签用于将符号标为已弃用；成员的其他性质则体现在详情中。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_tags -->

<!-- BEGIN CAPABILITY: supported -->

**已弃用符号**

已弃用的符号带有 Deprecated 标签，编辑器会为其加上删除线

```case
tests/document_symbols/symbol_tags/01_deprecated
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员标记**

成员带有 `static`、`virtual` 或 `= 0` 时会在详情中注明；协议的标签只表达弃用状态，访问权限不作显示

```case
tests/document_symbols/symbol_tags/02_modifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
