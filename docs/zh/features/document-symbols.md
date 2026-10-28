# 文档符号

<!-- The cards in the sections below are generated from the documented cases
     in tests/document_symbols/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

通过 `textDocument/documentSymbol` 提供文件大纲和面包屑导航：嵌套符号树包含范围、选择范围和 `detail` 字段，该字段用于区分重载并显示所声明的类型。

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

作用域外定义按代码中的书写位置列出，并显示其限定名；多次打开的命名空间每次打开都会单独列出

```case
tests/document_symbols/symbol_hierarchy/02_qualified_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**匿名作用域和内联作用域**

匿名命名空间、匿名结构体和匿名联合体的成员会归在各自的占位名称下，内联命名空间则保留自己的节点

```case
tests/document_symbols/symbol_hierarchy/03_anonymous_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**局部符号**

在函数体中声明的变量、类型和 Lambda 会嵌套在该函数之下

```case
tests/document_symbols/symbol_hierarchy/04_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**友元函数定义**

在类内定义的友元函数会列在该类之下

```case
tests/document_symbols/symbol_hierarchy/05_friend_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**访问控制区段**

成员按代码中的书写顺序列出；所属的 `public` 和 `private` 访问控制区段不构成大纲中的一层

```case
tests/document_symbols/symbol_hierarchy/06_access_sections
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#500 -->

**宏声明的成员**

一次宏调用以代码中没有写出的名称声明的内容会列在该调用之下，连同它在实参中命名的内容，例如测试宏生成的测试、类宏添加的成员；只声明实参中写出的名称的宏调用，则按书写形式列出这些名称

```case
tests/document_symbols/symbol_hierarchy/09_macro_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号种类

所有符号列出时都会标明其种类，包括命名空间、类、宏和模块声明。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**核心符号种类**

命名空间、类、联合体、枚举及其成员，以及函数、变量、字段、绑定和 Lambda 都会标明各自的种类

```case
tests/document_symbols/symbol_kinds/01_core_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

类模板、函数模板和变量模板的详细信息中会显示 `template`，其范围从 `template` 关键字开始；Concept 和简写模板（abbreviated templates）也会列出

```case
tests/document_symbols/symbol_kinds/02_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板特化和推导指引（deduction guides）**

模板特化会显示其模板实参及自身成员，推导指引则显示其推导出的签名

```case
tests/document_symbols/symbol_kinds/03_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

`typedef` 和 `using` 别名以及别名模板都会作为类型别名列出

```case
tests/document_symbols/symbol_kinds/04_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**显式实例化**

类模板、函数模板和变量模板的显式实例化（explicit instantiation）都会列出，但不包含成员

```case
tests/document_symbols/symbol_kinds/05_explicit_instantiations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**宏**

对象式宏和函数式宏都会列出，函数式宏还会在详细信息中显示其参数

```case
tests/document_symbols/symbol_kinds/06_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块声明**

模块单元会列出其声明或所属的模块

```case
tests/document_symbols/symbol_kinds/07_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**包含指令**

`#include` 指令不会列在大纲中；它们的文档链接会跳转到所指的文件

```case
tests/document_symbols/symbol_kinds/10_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pragma mark 区段**

`#pragma mark - Name` 开启一个区段，将其后的符号归入其中

```case
tests/document_symbols/symbol_kinds/11_pragma_marks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间别名**

命名空间别名作为命名空间列出，详细信息为 `namespace alias`

```case
tests/document_symbols/symbol_kinds/20_namespace_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件保护（include guards）**

头文件保护所用的宏不会列出，即包住整个头文件的 `#ifndef` 和 `#define`

```case
tests/document_symbols/symbol_kinds/21_include_guards
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号详细信息

名称旁的 `detail` 用于区分重载，并显示变量的声明类型。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**函数签名**

`detail` 字段显示函数的参数（包括参数名）和返回类型，借此区分重载；构造函数不显示返回类型

```case
tests/document_symbols/symbol_detail/01_detail_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量和字段类型**

变量或字段在详细信息中显示其声明类型，Lambda 则显示调用它时所接受的参数和返回类型

```case
tests/document_symbols/symbol_detail/02_variable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**默认实参**

函数的详细信息会列出参数及其默认值

```case
tests/document_symbols/symbol_detail/03_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#601 -->

**基类**

派生类的详细信息会按书写形式列出其基类，并带上访问权限

```case
tests/document_symbols/symbol_detail/04_base_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带作用域的类型**

类型名中写出的类作用域限定会在详细信息中保留，且只出现一次，待决名（dependent names）也不例外

```case
tests/document_symbols/symbol_detail/05_scoped_types
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号范围

每个符号都包含其完整声明的范围和名称范围；选择该符号时，编辑器会选中其名称。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**名称与声明范围**

符号的声明范围涵盖整个声明，名称范围涵盖代码中写出的完整名称，包括 `~Widget`、`operator==` 和 `operator bool`

```case
tests/document_symbols/symbol_ranges/01_selection_ranges
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**多行签名**

跨行声明的范围从第一行延伸至最后一行

```case
tests/document_symbols/symbol_ranges/02_multiline_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**宏声明的符号**

宏调用声明的符号列在调用位置，而非宏定义处

```case
tests/document_symbols/symbol_ranges/03_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**宏实参中的名称**

写在宏实参中的名称用作符号名称；若名称写在宏体中，则回退到宏调用处

```case
tests/document_symbols/symbol_ranges/04_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号标签

标签用于标记符号已弃用；成员的其他属性写在它的详细信息里。

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_tags -->

<!-- BEGIN CAPABILITY: supported -->

**已弃用的符号**

已弃用的符号带有 Deprecated 标签，编辑器会以删除线显示

```case
tests/document_symbols/symbol_tags/01_deprecated
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员标记**

成员为 `static`、`virtual` 或 `= 0` 时，其详细信息会注明；协议的标签只能表达弃用，访问权限不显示

```case
tests/document_symbols/symbol_tags/02_modifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
