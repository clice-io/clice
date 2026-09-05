# 悬停

为光标所在的符号显示内容丰富的信息卡片。

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## 符号信息

<!-- BEGIN GENERATED ITEMS: symbol_information -->

<!-- BEGIN CAPABILITY: supported -->

**限定名**

悬停卡片显示符号所在的命名空间和类作用域

```snap
tests/snap/hover/symbol_information/01_qualified_name.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**符号种类**

卡片标明符号的种类：结构体、枚举、函数、字段等

```snap
tests/snap/hover/symbol_information/02_symbol_kind.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**访问说明符**

成员显示其 public / protected / private 访问权限

```snap
tests/snap/hover/symbol_information/03_access_specifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**定义显示**

卡片包含符号在源代码中的定义

```snap
tests/snap/hover/symbol_information/04_definition_rendering.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#710 -->

**初始化器截断**

过大的初始化器会截断显示，不会完整呈现

显示的定义省略了初始化器，但求值后的 `Value` 字段仍会列出全部 256 个元素。

```snap
tests/snap/hover/symbol_information/05_initializer_truncation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2474 -->

**虚函数修饰符**

方法的悬停卡片显示 `virtual` / `override` / `final`

源代码中写出的修饰符会显示出来（`virtual … = 0`、`override`、`final`），但如果重写方法省略了多余的 `virtual` 关键字，卡片就不会表明它是虚函数，也就无法呈现该 issue 要求的 `virtual void draw() override` 形式。

```snap
tests/snap/hover/symbol_information/06_virtual_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#436 -->

**匿名命名空间作用域**

作用域中显示 `(anonymous namespace)`

卡片可以显示，但作用域会省略匿名部分：顶层匿名命名空间中的成员完全没有作用域一行，而 `outer::(anonymous)` 只显示为 `outer`。

```snap
tests/snap/hover/symbol_information/07_anon_namespace_scope.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型信息

<!-- BEGIN GENERATED ITEMS: type_information -->

<!-- BEGIN CAPABILITY: supported -->

**变量类型**

变量的悬停卡片保留指针、引用和数组声明符

变量卡片以易读的格式显示其声明类型，指针、引用和数组声明符的写法与源代码一致。

```snap
tests/snap/hover/type_information/01_variable_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

悬停卡片可以用 `aka` 形式显示去除语法糖后的类型

带有语法糖的类型会以 `Alias (aka int)` 的形式显示其底层类型。`show_aka` 选项可以关闭 `aka` 后缀。

```snap
tests/snap/hover/type_information/02_aka_desugar.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数签名**

函数的悬停卡片显示返回类型、参数名和默认实参

函数卡片列出返回类型、每个参数及其名称，以及所有默认实参。

```snap
tests/snap/hover/type_information/03_function_signature.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板形参**

模板的悬停卡片区分类型形参、模板模板形参和非类型形参

各类模板形参按各自的形式显示：类型形参、模板模板形参，以及带有默认值的非类型形参。

```snap
tests/snap/hover/type_information/04_template_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 推导**

占位符的悬停卡片显示推导出的类型

将光标悬停在 `auto` 占位符上，会显示替换它的类型，包括内置类型、指针、Lambda 和模板实例化类型；在尚未实例化的模板中，则显示 `/* not deduced */` 标记。

```snap
tests/snap/hover/type_information/05_auto_deduction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` 推导**

decltype 的悬停卡片区分值类型、引用类型和依赖形式

将光标悬停在 `decltype` 或 `decltype(auto)` 占位符上，会显示解析得到的类型，包括根据带括号表达式规则添加的引用。

```snap
tests/snap/hover/type_information/06_decltype_deduction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#435 -->

**CTAD**

类占位符的悬停卡片显示推导出的模板实参

使用类模板实参推导（CTAD）时，变量卡片会显示推导出的 `Box<int>`，但将光标悬停在类名上时，仍然只显示不带实参的主模板。

```snap
tests/snap/hover/type_information/07_ctad_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#230 -->

**实例化实参**

模板使用处的悬停卡片尚不显示形参与实参的绑定关系

在模板使用处，卡片会显示替换后的类型（`Wrapper<int>`、`identity<int>`、`int x`），但不会以 `T = int` 这样的形式明确列出各形参与其绑定实参的映射关系。

```snap
tests/snap/hover/type_information/08_instantiation_args.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#493 -->

**Lambda 的 `auto` 参数**

泛型 Lambda 参数尚不显示推导出的类型

悬停于泛型 Lambda 的 `auto` 参数时，不会显示卡片，也不会显示推导出的参数类型。

```snap
tests/snap/hover/type_information/09_lambda_auto_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**保留别名的 `auto`**

`auto` 推导后仍保留别名的写法

悬停时保留别名的写法，并附上展开别名后的类型，因此从别名返回类型推导出的 `auto` 会显示为 `Outer // aka: int`。

```snap
tests/snap/hover/type_information/10_sugared_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2156 -->

**类型格式化**

显示较长或嵌套的类型时，不会重新换行或对齐

较长或嵌套的类型不会重新换行或对齐。

```snap
tests/snap/hover/type_information/11_clang_format_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2219 -->

**匿名结构体的 typedef**

悬停于 C 语言的 typedef 时，以别名作为匿名结构体的名称

悬停于别名或该类型的变量时，都会显示简洁的 `Point` 卡片，而不会将匿名类型显示为另有名称的结构体。

```snap
tests/snap/hover/type_information/12_c_typedef_anon.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Concept 约束**

悬停于受约束的占位符时，约束信息会丢失

受约束参数和 Concept 引用的卡片都包含约束信息，但悬停于受约束的 `Addable auto` 变量的占位符时，只显示推导出的类型，约束信息会丢失。

```snap
tests/snap/hover/type_information/13_concept_constraints.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 布局信息

<!-- BEGIN GENERATED ITEMS: layout_information -->

<!-- BEGIN CAPABILITY: supported -->

**字段布局**

悬停于字段时，显示大小、偏移量、对齐和填充信息

```snap
tests/snap/hover/layout_information/01_field_layout.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1763 -->

**类型布局**

悬停于类型本身时，显示其大小、对齐和填充信息

目前类型卡片会显示大小和对齐信息，尚不显示总填充大小。

```snap
tests/snap/hover/layout_information/02_type_layout.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1771 -->

**虚函数表偏移量**

虚方法会显示其在虚函数表中的槽位

目前方法卡片不显示任何虚函数表信息。

```snap
tests/snap/hover/layout_information/03_vtable_offset.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 表达式上下文

<!-- BEGIN GENERATED ITEMS: expression_context -->

<!-- BEGIN CAPABILITY: supported -->

**常量求值**

悬停于常量表达式时，显示求值结果

当初始化器是常量表达式时，卡片会对其求值并显示结果。

```snap
tests/snap/hover/expression_context/01_constant_value.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**调用实参**

悬停于实参时，标明其绑定的形参

悬停于调用处的实参时，显示它所传入的形参，并给出所绑定形参的名称。

```snap
tests/snap/hover/expression_context/02_callee_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传递语义**

悬停于实参时，区分值传递和引用传递

实参卡片会说明值如何传递给被调用方：按值复制，或绑定到可修改的引用形参或常量引用形参。

```snap
tests/snap/hover/expression_context/03_pass_semantics.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**隐式转换**

悬停于实参时，显示隐式转换的目标类型

当实参通过隐式转换传递给形参时，卡片会注明目标类型，内置转换和用户定义转换均如此。

```snap
tests/snap/hover/expression_context/04_implicit_conversion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1016 -->

**字符串字面量**

悬停于字符串字面量时，显示其字节大小

字符串字面量卡片会显示数组类型及其字节大小（`const char[6]`、`Size: 6 bytes`，即字符串长度加上空字符终止符），而不会明确显示字符数。

```snap
tests/snap/hover/expression_context/05_string_length.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1669 -->

**数值字面量**

悬停于数值字面量时，尚不显示类型或值

悬停于数值字面量时不会显示卡片，而字符字面量和字符串字面量则会显示类型和值。

```snap
tests/snap/hover/expression_context/06_numeric_literal_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1622 -->

**记录类型变量**

悬停于记录类型时，可能会显示所属外层表达式的常量值，造成误导

对于可在编译期求值的调用，目前悬停于其记录类型实参时，变量上会显示该调用的值（`Value = 7`），而这个值并非记录本身的值。

```snap
tests/snap/hover/expression_context/07_record_value_misleading.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档

<!-- BEGIN GENERATED ITEMS: documentation -->

<!-- BEGIN CAPABILITY: supported -->

**Doxygen `///` 注释**

悬停时显示声明的文档

适用于普通函数、主模板及其特化；引用会解析到特化程度最高的声明所附的注释。

```snap
tests/snap/hover/documentation/01_doxygen_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**合成访问器文档**

为简单的 getter/setter 生成单行描述

如果简单的 getter 或 setter 没有自己的注释，悬停卡片中会自动生成一行 "Trivial accessor/setter for `field`." 描述。

```snap
tests/snap/hover/documentation/02_accessor_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1320 -->

**`@copydoc` 标签**

复制的文档尚未解析并关联到接收文档的符号

`@copydoc target` 标签会保留原文，不会将 `target` 的文档复制到当前符号的悬停卡片中。

```snap
tests/snap/hover/documentation/03_copydoc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2504 -->

**重写方法的文档继承**

没有注释的重写方法尚不能继承基类方法的文档

如果重写方法没有自己的注释，其悬停卡片中就没有描述。

```snap
tests/snap/hover/documentation/04_inherit_overridden_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2506 -->

**重载间的文档共享**

后续重载尚不能复用第一个重载的文档

连续声明的重载通常只为第一个编写文档；后续没有文档的重载在悬停卡片中没有描述。

```snap
tests/snap/hover/documentation/05_overload_doc_sharing.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1936 -->

**继承构造函数的文档**

继承构造函数的文档尚不能在悬停时显示

`using Base::Base;` 声明中的名称会解析到类，而非继承的构造函数，因此无法通过悬停查看基类构造函数的文档。

```snap
tests/snap/hover/documentation/06_inherited_ctor_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#974 -->

**横幅注释**

用空行隔开的分节横幅注释仍会关联到后面的声明

`// ==== Section ====` 横幅注释即使后面有空行，也会被错误地当作下方声明的文档，因此横幅文本会出现在卡片中。

```snap
tests/snap/hover/documentation/07_comment_association.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义处的注释**

声明处的文档优先于定义处的注释

在声明和定义处悬停时，显示的都是声明处的 `///` 文档，而非定义处的普通 `//` 注释。

```snap
tests/snap/hover/documentation/08_decl_vs_def_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2057 -->

**空白与换行**

注释中的 Markdown 表格在悬停时会丢失换行

写在多行 `///` 注释中的 Markdown 表格会被合并为一行，导致表格无法渲染。

```snap
tests/snap/hover/documentation/09_whitespace_preserve.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1040 -->

**注释缩进**

文档中缩进的块在悬停时会丢失行首缩进

如果文档注释的正文包含缩进块，该块的缩进会丢失，空行也会被合并掉。

```snap
tests/snap/hover/documentation/10_comment_indentation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1226 -->

**宏生成的模板关键字**

宏生成的模板在悬停时不显示文档

当 `template` 关键字由宏展开生成时，声明的文档注释不会在悬停时显示，卡片中没有描述。

```snap
tests/snap/hover/documentation/11_macro_template_doc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2148 -->

**注释屏蔽选项**

尚不能通过配置屏蔽错误关联的文档

与代码用空行隔开的分节横幅等无关注释总会出现在悬停卡片中，且没有选项可以屏蔽。

```snap
tests/snap/hover/documentation/12_comment_suppression.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏悬停

<!-- BEGIN GENERATED ITEMS: macro_hover -->

<!-- BEGIN CAPABILITY: supported -->

**各处均显示定义文本**

在 `#define`、使用处、`#ifdef` 和 `#undef` 处均显示宏定义

无论宏名称出现在何处，其悬停卡片都会显示对应的 `#define` 文本：包括宏定义本身、使用处、`#ifdef` 条件判断处以及 `#undef` 处。

```snap
tests/snap/hover/macro_hover/01_macro_definition_sites.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**完全展开预览**

在函数式宏的使用处显示实参代入宏体后的结果

悬停于函数式宏调用时，会显示 `#define` 文本，以及代入调用实参后完全展开的结果预览。

```snap
tests/snap/hover/macro_hover/02_expansion_preview.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命令行宏**

悬停于通过 `-D` 定义的宏时，会显示合成的 `#define`

对于在命令行中定义的宏（`-DFROM_CLI=7`），悬停卡片会先显示合成的 `#define FROM_CLI 7`，再显示其展开结果。

```snap
tests/snap/hover/macro_hover/03_cli_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**实参中的嵌套宏**

作为实参嵌套的宏只显示定义，不显示展开预览

展开预览从外层调用开始，因此悬停于实参中的内层宏名称时，只显示其定义，不显示展开预览。

```snap
tests/snap/hover/macro_hover/04_nested_arg_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2642 -->

**在定义前使用**

在定义前使用的宏尚无悬停信息

在宏自身的 `#define` 之前的 `#if` 中使用该宏名称时，没有悬停信息；在定义后使用时，悬停功能正常。

```snap
tests/snap/hover/macro_hover/05_expansion_before_definition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Preamble 中的 `#define`**

文件开头的宏定义没有悬停卡片

首个声明之前、文件开头连续指令中的 `#define` 没有悬停卡片，而声明之后的宏定义有。

```snap
tests/snap/hover/macro_hover/06_preamble_define_hover.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 特殊悬停目标

<!-- BEGIN GENERATED ITEMS: special_hover_targets -->

<!-- BEGIN CAPABILITY: partial clangd#959 -->

**类型悬停中的成员**

悬停于枚举或结构体类型时，会列出其成员

卡片显示类型名称（对于结构体，还会显示其布局），但不展开成员列表，类型体显示为 `{}`。

```snap
tests/snap/hover/special_hover_targets/01_type_members_on_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2020 -->

**typedef 的底层结构体**

悬停于别名时，会展开其所指类型的定义

卡片将别名解析为其底层类型名称，但不展开该结构体的定义或成员列表。

```snap
tests/snap/hover/special_hover_targets/02_typedef_underlying.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1862 -->

**关键字文档**

语言关键字尚无文档悬停信息

悬停于 `const` 或 `virtual` 等关键字时，不会显示卡片。

```snap
tests/snap/hover/special_hover_targets/03_keyword_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1862 -->

**属性文档**

悬停于属性时，会显示其说明

无论是 GNU `__attribute__` 写法还是 C++ `[[...]]` 属性，卡片都会显示属性自身的文档。

```snap
tests/snap/hover/special_hover_targets/04_attributes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**包含指令悬停**

悬停于 `#include` 时，会显示解析后的头文件路径

卡片将引号中的头文件解析为磁盘上对应的文件。

```snap
tests/snap/hover/special_hover_targets/05_include_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this` 表达式**

悬停于 `this` 时，会显示其指向的类类型

在普通类和类模板中均可使用。

```snap
tests/snap/hover/special_hover_targets/06_this_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预定义标识符**

悬停于 `__func__` 时，会显示当前函数名

在具体函数中可以解析出其值；在模板中，只能得知其近似类型。

```snap
tests/snap/hover/special_hover_targets/07_predefined_identifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无意义的 Token 不显示悬停信息**

内置关键字和空的主体不显示卡片

悬停于内置类型关键字或空的主体内部时，完全不会显示卡片，因此编辑器不会显示任何内容，避免产生干扰。（数值和布尔字面量目前也没有卡片，但这是已记录的功能缺失，参见数值字面量条目，并不保证今后也如此。）

```snap
tests/snap/hover/special_hover_targets/08_no_hover_negatives.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2662 -->

**GTK-Doc 和 kernel-doc**

GObject Introspection 注解尚未显示在悬停卡片中

GTK-Doc 和 kernel-doc 的注释语法以及 GObject Introspection 注解尚未解析并显示在悬停卡片中。

```snap
tests/snap/hover/special_hover_targets/09_gtk_doc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2669 -->

**Doxygen 中的 LaTeX 数学公式**

Doxygen 行内公式不会渲染为数学公式

公式文本不会渲染为数学公式。

```snap
tests/snap/hover/special_hover_targets/10_latex_math.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 展示

<!-- BEGIN GENERATED ITEMS: presentation -->

<!-- BEGIN CAPABILITY: supported -->

**Markdown 渲染**

卡片以 Markdown 渲染，也可通过 `parse_comment_as_markdown =
false` 以纯文本显示

```snap
tests/snap/hover/presentation/01_presentation.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块相关

<!-- BEGIN GENERATED ITEMS: module_related -->

<!-- BEGIN CAPABILITY: unsupported -->

**导入语句悬停**

悬停于 `import` 时尚不显示导入模块的说明

悬停于 `import` 声明时，尚不显示导入模块的说明。

```snap
tests/snap/hover/module_related/01_import_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**模块名悬停**

悬停于模块名时尚不列出其所属文件

悬停于模块名时，尚不列出声明该模块的文件或分区。

```snap
tests/snap/hover/module_related/02_module_name_hover.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 悬停正确性

面对曾导致其他工具出错的输入时仍保持稳健。

<!-- BEGIN GENERATED ITEMS: hover_correctness -->

<!-- BEGIN CAPABILITY: supported -->

**MSVC 继承模型**

MSVC 继承模型属性不会改变类类型的悬停信息

以 MSVC 为编译目标时，隐式继承属性不会出现在类类型或方法的悬停卡片中。

```snap
tests/snap/hover/hover_correctness/01_ms_inheritance.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**最令人烦恼的解析（most vexing parse）**

直接初始化与函数声明的悬停卡片不同

直接初始化显示为变量，而最令人烦恼的解析形式显示为函数声明。

```snap
tests/snap/hover/hover_correctness/02_object_vs_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**大数值无符号枚举常量**

悬停于值为 `0xFFFF...ULL` 的枚举项时不会崩溃

卡片会完整显示无符号数值，不会溢出或出错。

```snap
tests/snap/hover/hover_correctness/03_large_enum_value.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**使用默认实参的调用**

悬停于省略默认实参的调用时不会崩溃

调用的悬停卡片会显示被调用函数的签名，包含默认实参。

```snap
tests/snap/hover/hover_correctness/04_default_args_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**被宏遮蔽的符号**

函数式宏在调用处遮蔽同名函数

卡片会显示生效的宏及其展开结果，而非被遮蔽的函数。

```snap
tests/snap/hover/hover_correctness/05_macro_shadowed_symbol.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
