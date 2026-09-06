# 文档符号

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

通过 `textDocument/documentSymbol` 提供文件大纲和面包屑导航：嵌套符号树包含范围、选择范围和 `detail` 字段，该字段用于区分重载并显示所声明的类型。

## 符号层级

<!-- BEGIN GENERATED ITEMS: symbol_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套符号树**

符号按代码中书写的作用域嵌套；作用域外的定义以限定名称显示在其实际书写的位置

```snap
tests/snap/document_symbol/symbol_hierarchy/01_hierarchy_nesting.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**符号范围和选择范围**

符号范围覆盖整个声明；选择范围覆盖完整书写的名称，包括 `~Widget`、`operator==` 和 `operator bool` 等由多个 Token 组成的名称

```snap
tests/snap/document_symbol/symbol_hierarchy/02_hierarchy_selection_ranges.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#499 -->

**按访问说明符分组**

访问说明符尚未在大纲中形成分组节点

```snap
tests/snap/document_symbol/symbol_hierarchy/03_hierarchy_access_specifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**匿名作用域和内联作用域**

匿名命名空间、未命名结构体和联合体的成员归在占位名称下；内联命名空间的成员仍归在该内联命名空间节点下

```snap
tests/snap/document_symbol/symbol_hierarchy/04_hierarchy_anonymous.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**UTF-16 位置编码**

非 ASCII 文本之后的列位置按 UTF-16 代码单元计数

```snap
tests/snap/document_symbol/symbol_hierarchy/05_hierarchy_utf16.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号种类

<!-- BEGIN GENERATED ITEMS: symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**核心符号种类**

命名空间、类、结构体、联合体、枚举及其成员、函数、变量、字段、结构化绑定（structured bindings）和 Lambda 都会出现在大纲中，并映射到相应的 LSP 符号种类

```snap
tests/snap/document_symbol/symbol_kinds/01_kinds_basic.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板声明**

类模板、函数模板和变量模板的详细信息带有 `template ` 前缀；Concept 和简写函数模板（使用 `concept auto` 参数）也会出现在大纲中

```snap
tests/snap/document_symbol/symbol_kinds/02_kinds_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板特化和推导指引**

类模板和变量模板的显式特化与偏特化会在名称中显示模板实参；成员嵌套在所属特化下；推导指引显示推导出的签名

```snap
tests/snap/document_symbol/symbol_kinds/03_kinds_specializations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

`typedef`、`using` 别名和别名模板会出现在大纲中，详细信息为 `type alias`

```snap
tests/snap/document_symbol/symbol_kinds/04_kinds_type_aliases.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**显式实例化指令**

类的显式实例化显示为没有子节点的符号，而函数和变量的显式实例化尚未出现在大纲中

```snap
tests/snap/document_symbol/symbol_kinds/05_kinds_explicit_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**宏定义**

对象式宏和函数式宏会出现在大纲中，函数式宏的详细信息还会列出参数

```snap
tests/snap/document_symbol/symbol_kinds/06_kinds_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Preamble 区域中的宏**

对于编辑器发起的请求，文件开头连续预处理指令中的宏尚未列入大纲

```snap
tests/snap/document_symbol/symbol_kinds/07_macro_preamble.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号详细信息

<!-- BEGIN GENERATED ITEMS: symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**函数签名**

`detail` 字段中的参数类型和返回类型用于区分重载；构造函数省略 `void` 返回类型

```snap
tests/snap/document_symbol/symbol_detail/01_detail_signatures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量和字段类型**

变量的详细信息显示声明的类型，Lambda 则显示为 `(lambda)`

```snap
tests/snap/document_symbol/symbol_detail/02_detail_variable_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#221 -->

**移除默认实参**

签名根据函数类型生成，因此参数默认值不会出现在大纲中

```snap
tests/snap/document_symbol/symbol_detail/03_detail_default_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**详细信息中的基类**

派生类的详细信息尚未包含其基类

```snap
tests/snap/document_symbol/symbol_detail/04_detail_base_classes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**多行签名范围**

符号范围从声明起始处开始，覆盖完整签名，使编辑器的粘性滚动能够正确定位

```snap
tests/snap/document_symbol/symbol_detail/05_detail_multiline_signatures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带作用域的类型**

代码中书写的类作用域在详细信息中恰好出现一次，嵌套类、模板标识（template-id）、别名和依赖名称均如此

```snap
tests/snap/document_symbol/symbol_detail/06_detail_scoped_types.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 缺失的符号

<!-- BEGIN GENERATED ITEMS: missing_symbols -->

<!-- BEGIN CAPABILITY: unsupported clangd#2226 -->

**包含指令**

`#include` 指令尚未出现在大纲中

```snap
tests/snap/document_symbol/missing_symbols/01_missing_includes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**局部符号**

函数体内声明的变量和类型嵌套在所属函数下

```snap
tests/snap/document_symbol/missing_symbols/02_local_symbols.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**模块声明**

模块声明尚未显示在大纲中

```snap
tests/snap/document_symbol/missing_symbols/03_missing_modules.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`#pragma mark` 导航标记**

编辑器分节标记尚未显示在大纲中

```snap
tests/snap/document_symbol/missing_symbols/04_missing_pragma_mark.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**友元函数定义**

在类中内联定义的友元函数显示在该类下

```snap
tests/snap/document_symbol/missing_symbols/05_friend_definitions.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 符号标签

<!-- BEGIN GENERATED ITEMS: symbol_tags -->

<!-- BEGIN CAPABILITY: unsupported -->

**弃用标签**

已弃用的符号尚未带有 LSP `deprecated` 符号标签

```snap
tests/snap/document_symbol/symbol_tags/01_tags_deprecated.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2123 -->

**访问权限与存储属性标识**

大纲条目尚未显示访问权限、静态、虚或抽象修饰信息

```snap
tests/snap/document_symbol/symbol_tags/02_tags_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 位置准确性

<!-- BEGIN GENERATED ITEMS: location_correctness -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**宏展开生成的符号**

宏调用生成的符号定位到宏调用处，而非宏定义处

```snap
tests/snap/document_symbol/location_correctness/01_macro_symbols.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**宏实参中书写的名称**

选择范围指向宏实参中书写的名称；对于宏体中书写的名称，则回退到宏调用处

```snap
tests/snap/document_symbol/location_correctness/02_macro_argument_names.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
