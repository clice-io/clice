# 悬停

为光标所在的符号显示内容丰富的信息卡片。

<!-- The cards in the sections below are generated from the documented cases
     in tests/hover/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 符号

卡片显示符号的种类、声明所在的作用域及其声明。

<!-- BEGIN GENERATED ITEMS: hover/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**符号种类**

卡片标题标明符号的种类：命名空间、结构体、联合体、枚举、枚举项、别名、字段、函数

```case
tests/hover/symbols/01_symbol_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域**

卡片指出符号声明所在的命名空间、类或函数

```case
tests/hover/symbols/02_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**访问权限**

成员的定义会显示其 `public`、`protected` 或 `private` 访问权限

```case
tests/hover/symbols/03_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**定义**

无论在符号的定义处还是使用处，卡片都会以源代码形式显示其声明

```case
tests/hover/symbols/04_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2474 -->

**虚函数**

成员函数的定义按源码显示 `virtual`、`override` 或 `final`，因此重写的成员函数若未写出这些关键字，就不会显示出它是虚函数

```case
tests/hover/symbols/05_virtual_modifiers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#436 -->

**匿名命名空间**

卡片中的作用域省略匿名命名空间

```case
tests/hover/symbols/06_anonymous_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**已弃用的符号**

已弃用函数的卡片目前尚未标明该函数已弃用

`[[deprecated]]` 的消息说明应改用什么；卡片既不显示弃用状态，也不显示该消息。

```case
tests/hover/symbols/13_deprecation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件中的符号**

对于所包含头文件中声明的符号，卡片会显示其文档和定义

```case
tests/hover/symbols/14_header_symbols
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 函数

函数卡片列出返回类型和参数。

<!-- BEGIN GENERATED ITEMS: hover/functions -->

<!-- BEGIN CAPABILITY: supported -->

**函数签名**

函数卡片列出返回类型和各个参数，并显示参数的名称与默认值

```case
tests/hover/functions/01_signatures
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 变量

变量卡片按源码显示变量的类型；若其值在编译时已知，也会显示该值。

<!-- BEGIN GENERATED ITEMS: hover/variables -->

<!-- BEGIN CAPABILITY: supported -->

**声明符类型**

变量卡片按源码显示其指针、引用和数组声明符

```case
tests/hover/variables/01_declarators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**常量值**

变量或枚举项的值若在编译时已知，就会显示该值

```case
tests/hover/variables/02_constant_values
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#710 -->

**较长的初始化器**

定义中会省略较长的初始化器，值则只显示开头的几个元素

```case
tests/hover/variables/04_long_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__func__`**

若所在函数不是模板，将光标悬停在 `__func__` 上会显示该函数的名称

```case
tests/hover/variables/06_predefined_identifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型成员

类会列出其成员，枚举会列出其枚举项。

<!-- BEGIN GENERATED ITEMS: hover/members -->

<!-- BEGIN CAPABILITY: supported clangd#959 -->

**类型成员**

类会列出其数据成员和成员类型，枚举会列出其枚举项及相应的值

`max_members` 限制列表中的成员数量，默认值为 20；设为 `0` 时仅显示声明。

```case
tests/hover/members/01_type_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型别名

以别名表示的类型会显示该别名所代表的类型。

<!-- BEGIN GENERATED ITEMS: hover/aliases -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

以别名表示的类型会显示该别名所代表的类型，格式为 `Alias (aka int)`

设置 `show_aka = false` 后，卡片仅保留别名。

```case
tests/hover/aliases/01_aka
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2020 -->

**结构体别名**

别名会显示其所代表的结构体名称，但目前尚不显示结构体的成员

```case
tests/hover/aliases/02_alias_targets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2219 -->

**C 中匿名结构体的 typedef**

在 C 中，仅通过 typedef 命名的结构体会以该 typedef 的名称显示

定义行仍写作 `struct Point`，但在 C 中，该结构体并无此名称。

```case
tests/hover/aliases/03_c_anonymous_structs
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

`auto` 和 `decltype` 会显示其代表的类型。

<!-- BEGIN GENERATED ITEMS: hover/deduced_types -->

<!-- BEGIN CAPABILITY: partial -->

**`auto`**

将光标悬停在 `auto` 上会显示其代表的类型；若在模板中尚未推导出类型，则会显示相应提示

```case
tests/hover/deduced_types/01_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype`**

将光标悬停在 `decltype` 上会显示其得出的类型，包括为名称加上括号所引入的引用

```case
tests/hover/deduced_types/02_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过别名推导的 auto**

从别名推导出的 `auto` 会在其代表的类型旁保留别名的名称

```case
tests/hover/deduced_types/03_alias_spelling
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#493 clangd#1768 -->

**泛型 Lambda 参数**

泛型 Lambda 的 `auto` 会显示调用时推导出的类型，但形参及其成员不会显示该类型

在函数体内，形参显示为 `auto`，`point.x` 没有悬停卡片。

```case
tests/hover/deduced_types/06_generic_lambdas
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板

模板形参会显示其种类和默认值，模板的使用处会显示实例化时采用的实参。

<!-- BEGIN GENERATED ITEMS: hover/templates -->

<!-- BEGIN CAPABILITY: supported -->

**模板形参**

模板形参会显示其种类（类型、模板或值）及默认值

```case
tests/hover/templates/01_template_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#230 -->

**模板实参**

模板的使用处会显示实例化时采用的类型，但尚未显示各实参绑定到哪个形参

```case
tests/hover/templates/02_instantiation_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#435 -->

**推导出的类模板实参**

类模板实参通过推导确定的变量会显示这些实参，而旁边的类名只显示模板本身

```case
tests/hover/templates/03_deduced_class_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Concept 约束**

Concept 和受约束的形参会显示约束，但 `Addable auto` 占位符只显示推导出的类型

```case
tests/hover/templates/04_concept_constraints
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 布局

字段和类会显示编译器为其确定的大小和对齐要求。

<!-- BEGIN GENERATED ITEMS: hover/layout -->

<!-- BEGIN CAPABILITY: supported -->

**字段布局**

字段会显示其偏移量、大小、对齐要求以及后面的填充

```case
tests/hover/layout/01_field_layout
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1763 -->

**类型布局**

类会显示其大小和对齐要求，但尚未显示其中有多少空间用于填充

```case
tests/hover/layout/02_type_layout
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1771 -->

**虚函数表（vtable）槽位**

虚成员函数尚未显示其在虚函数表中的槽位

```case
tests/hover/layout/03_vtable_slots
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用实参

调用处的实参会显示对应的形参和传递方式。

<!-- BEGIN GENERATED ITEMS: hover/arguments -->

<!-- BEGIN CAPABILITY: supported -->

**调用实参**

调用处的实参会显示对应的形参

```case
tests/hover/arguments/01_bound_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传递方式**

实参的悬停卡片会注明它是按值、按引用还是按常量引用传递

```case
tests/hover/arguments/02_pass_modes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**隐式转换**

实参若在传给形参的过程中发生转换，会显示转换后的类型

```case
tests/hover/arguments/03_conversions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1622 -->

**记录类型（record）实参**

若调用在编译期求值，传入的记录类型实参会将该调用的值显示为自身的值

```case
tests/hover/arguments/04_record_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 表达式

字面量和 `this` 会显示各自的信息。

<!-- BEGIN GENERATED ITEMS: hover/expressions -->

<!-- BEGIN CAPABILITY: unsupported clangd#1016 clangd#1669 -->

**字面量**

字符串字面量会显示其类型和大小，而数值字面量尚未显示任何信息

大小包括末尾的空字符；不显示字符串的长度。

```case
tests/hover/expressions/01_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this`**

在类和类模板中，悬停在 `this` 上会显示它所指向的类型

```case
tests/hover/expressions/02_this
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

宏会显示其 `#define`，宏的使用处会显示展开结果。

<!-- BEGIN GENERATED ITEMS: hover/macros -->

<!-- BEGIN CAPABILITY: partial clangd#2642 -->

**在定义前使用**

宏名在自身的 `#define` 之前使用时没有悬停卡片，在之后使用时则有

```case
tests/hover/macros/01_use_before_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏的各个出现位置**

定义处、使用处、`#ifdef`、`defined` 和 `#undef` 处都会显示宏的 `#define`

```case
tests/hover/macros/02_definition_sites
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开预览**

函数式宏的使用处会显示代入实参后的展开结果

```case
tests/hover/macros/03_expansion_preview
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命令行宏**

通过 `-D` 定义的宏会显示与之等效的 `#define`

```case
tests/hover/macros/04_command_line
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**实参中的宏**

位于另一个宏的实参中的宏会显示其定义，但不显示展开结果

```case
tests/hover/macros/05_nested_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 包含指令

`#include` 会显示解析后对应的文件。

<!-- BEGIN GENERATED ITEMS: hover/directives -->

<!-- BEGIN CAPABILITY: supported -->

**包含指令**

悬停在 `#include` 上会显示解析后对应的文件

```case
tests/hover/directives/01_include_paths
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档

无论在符号的哪个位置悬停，卡片都会显示其声明上的文档注释。

<!-- BEGIN GENERATED ITEMS: hover/documentation -->

<!-- BEGIN CAPABILITY: supported -->

**文档注释**

在声明处及每个使用处，悬停卡片都会显示该声明的 `///` 注释

模板的使用处会显示所选模板特化的注释。

```case
tests/hover/documentation/01_doc_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**取值函数（getter）和设值函数（setter）**

简单的取值函数或设值函数若没有自己的注释，会显示访问对应字段的描述

```case
tests/hover/documentation/02_accessors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1320 -->

**@copydoc**

`@copydoc` 命令按原样显示，不会显示它所引用的文档

```case
tests/hover/documentation/03_copydoc
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2504 -->

**重写**

重写函数没有自己的注释时，不会显示被重写函数的文档

```case
tests/hover/documentation/04_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2506 -->

**重载**

没有注释的重载不会显示前一个重载的文档

```case
tests/hover/documentation/05_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1936 -->

**继承构造函数**

`using Base::Base` 中的名称显示它继承的构造函数及其文档

```case
tests/hover/documentation/06_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#974 -->

**横幅注释**

声明上方的分节横幅注释会显示为该声明的文档，即使两者之间有空行

```case
tests/hover/documentation/07_banner_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**声明与定义**

声明的 `///` 注释会在声明和定义处显示，除非先在定义上悬停

修改后，如果先在定义上悬停，定义的普通 `//` 注释就会取而代之，在两处显示。

```case
tests/hover/documentation/08_declaration_and_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2057 -->

**Markdown 表格**

注释中的表格会挤在同一行显示

```case
tests/hover/documentation/09_markdown_tables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1040 -->

**缩进块**

注释中的缩进块会丢失缩进和前面的空行

```case
tests/hover/documentation/10_indented_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2148 -->

**游离注释**

与声明之间隔着空行的注释会显示为该声明的文档，且没有选项可以关闭这一行为

```case
tests/hover/documentation/11_stray_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2662 -->

**GTK-Doc 注释**

GTK-Doc 的名称行、参数条目和标注均按原样显示

```case
tests/hover/documentation/12_gtk_doc
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2669 -->

**公式**

Doxygen 公式显示为源码

```case
tests/hover/documentation/13_latex_formulas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Doxygen 命令**

`@brief`、`@param` 和 `@return` 按原样显示，尚未整合到卡片的相应部分

参数说明可以显示在卡片中对应参数的旁边，返回值说明也可以显示在返回类型的旁边；目前整段注释都以文本形式显示，包括其中的命令。

```case
tests/hover/documentation/18_doxygen_commands
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块名称处会显示其接口单元所在的文件，来自模块的声明处会显示该声明是否已导出，以及模块接口为其提供的文档。

<!-- BEGIN GENERATED ITEMS: hover/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块名称**

在 `import` 和模块声明中，模块名称处都会显示其接口单元所在的文件

```case
tests/hover/modules/01_module_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**导出的声明**

模块接口中的声明在被模块导出时会显示 `export`

```case
tests/hover/modules/02_export_status
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带文档的导出**

从模块导入的符号会显示模块接口中为其编写的文档

```case
tests/hover/modules/04_documented_exports
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 属性与关键字

属性以及 `const`、`virtual` 等关键字各有其含义，可由卡片加以说明。

<!-- BEGIN GENERATED ITEMS: hover/attributes -->

<!-- BEGIN CAPABILITY: unsupported clangd#1862 -->

**属性**

在属性上悬停会显示 clang 对该属性的说明，GNU 写法和标准写法均适用

说明仍以 clang 的 reStructuredText 源码形式显示，其中的代码示例挤在一起。

```case
tests/hover/attributes/01_attributes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1862 -->

**关键字**

在 `const`、`virtual` 等关键字上悬停时，目前还不会显示任何内容

```case
tests/hover/attributes/02_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 无内容可显示

没有值得用卡片展示的内容时，就不会出现卡片。

<!-- BEGIN GENERATED ITEMS: hover/no_card -->

<!-- BEGIN CAPABILITY: supported -->

**无内容可显示**

内置类型关键字或空代码块内部不会显示卡片

```case
tests/hover/no_card/01_meaningless_tokens
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 展示

卡片使用 Markdown 格式；对于无法显示 Markdown 的编辑器，则使用纯文本。

<!-- BEGIN GENERATED ITEMS: hover/presentation -->

<!-- BEGIN CAPABILITY: supported -->

**纯文本卡片**

卡片使用 Markdown 格式；对于无法显示 Markdown 的编辑器，则使用纯文本

```case
tests/hover/presentation/01_plain_text
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
