# 悬停

为光标所在的符号展示详尽的信息卡片。

<!-- The cards in the sections below are generated from the documented cases
     in tests/hover/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 符号

卡片标明符号的种类与声明所在的作用域，并展示其声明。

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

卡片指明符号声明所在的命名空间、类或函数

```case
tests/hover/symbols/02_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**访问权限**

成员的访问权限为 `protected` 或 `private` 时，卡片标题会予以标明

```case
tests/hover/symbols/03_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**定义**

无论在符号的定义处还是使用处，卡片都会以源码形式显示其声明

```case
tests/hover/symbols/04_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2474 -->

**虚函数**

成员函数的定义按源码写法显示 `virtual`、`override` 或 `final`；重写函数无论是否写出关键字，都会指明它重写的是哪个基类函数

```case
tests/hover/symbols/05_virtual_modifiers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#436 -->

**匿名命名空间**

卡片中的作用域会标出名称声明所在的匿名命名空间

```case
tests/hover/symbols/06_anonymous_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**已弃用的符号**

已弃用函数的卡片会标明已弃用，并附上弃用说明

`[[deprecated]]` 的消息通常会说明应当改用什么。

```case
tests/hover/symbols/13_deprecation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件中的符号**

所包含头文件中声明的符号会显示其文档和定义

```case
tests/hover/symbols/14_header_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Objective-C 符号**

悬停在 Objective-C 的类、协议、方法、属性或实例变量上时，会显示其声明及所属作用域

成员会标出声明它的类或分类（category），如 `Shape(Extras)`；`@implementation` 会显示它实现的 `@interface`。悬停在选择器的任意一段上都会显示对应的方法。

```case
tests/hover/symbols/18_objective_c
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 函数

函数的卡片显示其签名。

<!-- BEGIN GENERATED ITEMS: hover/functions -->

<!-- BEGIN CAPABILITY: supported -->

**函数签名**

函数的卡片显示其签名，每个形参都带有名称和默认值

```case
tests/hover/functions/01_signatures
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 变量

变量的卡片按源码写法显示其类型；若其值在编译期已知，还会一并显示该值。

<!-- BEGIN GENERATED ITEMS: hover/variables -->

<!-- BEGIN CAPABILITY: supported -->

**声明符类型**

变量的卡片按源码写法显示其指针、引用和数组声明符

```case
tests/hover/variables/01_declarators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**常量值**

值在编译期已知的变量或枚举项会显示其值

```case
tests/hover/variables/02_constant_values
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#710 -->

**较长的初始化器**

较长的初始化器不会完整写入定义，值一行只显示前几个元素

```case
tests/hover/variables/04_long_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__func__`**

悬停在 `__func__` 上时，若所在函数不是模板，会显示该函数的名称

```case
tests/hover/variables/06_predefined_identifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型成员

类会列出其数据成员和成员类型，枚举会列出其枚举项。

<!-- BEGIN GENERATED ITEMS: hover/members -->

<!-- BEGIN CAPABILITY: supported clangd#959 -->

**类型成员**

类会列出其数据成员和成员类型，枚举会列出其枚举项及对应的值

`max_members` 限制列出的成员数量，默认为 20；设为 `0` 时只显示声明本身。

```case
tests/hover/members/01_type_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型别名

用别名写出的类型会显示该别名所代表的目标类型。

<!-- BEGIN GENERATED ITEMS: hover/aliases -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

用别名写出的类型会标明它所代表的类型，形式如“`Alias` is `int`”

设为 `show_aka = false` 时，卡片只保留别名本身。

```case
tests/hover/aliases/01_aka
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2020 -->

**结构体别名**

结构体的别名会标出它所代表的结构体并显示其成员

```case
tests/hover/aliases/02_alias_targets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2219 -->

**C 中匿名结构体的 typedef**

在 C 中，仅靠 typedef 命名的结构体按该 typedef 的名称显示，形式如 `typedef struct { ... } Point`

```case
tests/hover/aliases/03_c_anonymous_structs
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

`auto` 和 `decltype` 会显示它们所代表的类型。

<!-- BEGIN GENERATED ITEMS: hover/deduced_types -->

<!-- BEGIN CAPABILITY: supported -->

**`auto`**

悬停在 `auto` 上会显示它所代表的类型；在模板中则按模板内部的写法显示

只有实例化才能算出的类型（如 `value + 1` 的类型）不会推导。

```case
tests/hover/deduced_types/01_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype`**

悬停在 `decltype` 上会显示它求出的类型，外加圆括号为名称引入的引用也包含在内

```case
tests/hover/deduced_types/02_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由别名推导的 auto**

从别名推导出的 `auto` 会同时保留别名名称和它所代表的类型

```case
tests/hover/deduced_types/03_alias_spelling
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#493 clangd#1768 -->

**泛型 Lambda 形参**

泛型 Lambda 的 `auto` 形参显示调用该 Lambda 时传入的类型，且形参的各个成员也都有悬停卡片

在函数体内，该形参显示为调用处的实参类型，`point.x` 对应那个类型的成员。以多种类型调用的 Lambda 则将形参保留为 `auto`。

```case
tests/hover/deduced_types/06_generic_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**库中的类型别名**

变量的类型会保留其初始化器写出的别名，类型特征的结果以及标准库内部私用的别名除外

`values.size()` 显示为 `std::vector<int>::size_type` 并在后面附上底层类型；来自 `__alloc_traits`、由 `front()` 返回的成员别名直接显示为 `int&`，类型特征的结果（如 `invoke_result_t<…>`）则显示为它算出的类型。包装了标准库内部私用类的别名会在后面标出那个类：`std::vector<int>::iterator` 对应 `__gnu_cxx::__normal_iterator<int*, std::vector<int>>`。绑定到这类类型的结构化绑定（structured bindings）会从初始化器继承别名：如 `position`（来自 `ids.insert(1)`）的类型显示为 `std::set<int>::iterator`。

```case
tests/hover/deduced_types/14_library_aliases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板

模板形参会显示其种类和默认值，模板的使用处会显示实例化所用的实参。

<!-- BEGIN GENERATED ITEMS: hover/templates -->

<!-- BEGIN CAPABILITY: supported -->

**模板形参**

模板形参显示其种类（类型、模板或值）及默认值

```case
tests/hover/templates/01_template_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板实参**

模板的使用处显示该模板及其实例化所用的实参

```case
tests/hover/templates/02_instantiation_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#435 -->

**推导出的类模板实参**

执行类模板实参推导（CTAD）的声明在类名处和变量处都会显示推导出的实参

```case
tests/hover/templates/03_deduced_class_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concept 约束**

Concept 和受约束的形参会显示约束条件；`Addable auto` 占位符会显示它推导出的具体类型

```case
tests/hover/templates/04_concept_constraints
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖特化中的成员**

经由类模板依赖特化访问的成员，按代码写出的模板实参显示

在 `lib::vector<Point<T>>` 上，`push_back` 归属于 `lib::vector<Point<T>>`，其 `value_type` 为 `Point<T>`，而不是模板自身的 `_Tp`；而 `second`（属于 `pair<const int, T>`，由 `map<int, T>` 查找返回）的类型为 `T`。

```case
tests/hover/templates/10_library_specialization_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**实例化来源**

实例化的使用处会显示实例化它的源模板及所绑定的实参

偏特化会显示该偏特化自身，类模板的成员会先显示所属类的模板实参。取自默认值的实参和空参数包（parameter pack）会省略；显式特化按源码中的写法原样显示。

```case
tests/hover/templates/12_instantiation_patterns
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 布局

无论在何处悬停，字段和类都会按编译器计算出的结果，显示其大小、对齐要求和填充字节。

<!-- BEGIN GENERATED ITEMS: hover/layout -->

<!-- BEGIN CAPABILITY: supported -->

**字段布局**

字段显示其偏移量、大小、对齐要求以及紧随其后的填充字节

`layout` 默认开启；设为 `false` 时隐藏这些信息，类的大小也一并隐藏。

```case
tests/hover/layout/01_field_layout
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1763 -->

**类型布局**

类显示其大小、对齐要求，以及总大小中填充字节所占的字节数

```case
tests/hover/layout/02_type_layout
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用实参

调用处的实参会显示它所对应的形参以及传递方式。

<!-- BEGIN GENERATED ITEMS: hover/arguments -->

<!-- BEGIN CAPABILITY: supported -->

**调用实参**

调用处的实参会显示它作为哪个形参传入

```case
tests/hover/arguments/01_bound_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传递方式**

实参的卡片会标明它是按值、按引用还是按 const 引用传递

```case
tests/hover/arguments/02_pass_modes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**隐式转换**

实参在传给形参时若发生隐式转换，会标出转换后的目标类型

```case
tests/hover/arguments/03_conversions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1622 -->

**记录类型实参**

传给编译期求值调用的记录类型（record）实参，不会把整个调用的求值结果误当作自身的值

```case
tests/hover/arguments/04_record_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 表达式

数值显示其类型和值，字符串显示其类型、长度和占用大小，`this` 显示它所指向的类型。

<!-- BEGIN GENERATED ITEMS: hover/expressions -->

<!-- BEGIN CAPABILITY: supported clangd#1016 clangd#1669 -->

**字面量**

数值显示其类型和值，字符串显示其类型、长度和占用大小

长度不计结尾的空字符，占用大小则将其计入。

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

宏会显示其 `#define` 定义，宏的使用处还会显示展开结果。

<!-- BEGIN GENERATED ITEMS: hover/macros -->

<!-- BEGIN CAPABILITY: supported clangd#2642 -->

**先于定义使用**

宏名出现在自身的 `#define` 上方时，仍会显示该定义及所在行号

```case
tests/hover/macros/01_use_before_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏的各个出现位置**

定义处、使用处、`#ifdef`、`defined` 和 `#undef` 处都会显示该宏的 `#define`

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

用 `-D` 定义的宏会显示与之等价的 `#define`

```case
tests/hover/macros/04_command_line
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**实参中的宏**

写在另一个宏实参内部的宏，会显示其定义及在该处的展开结果

```case
tests/hover/macros/05_nested_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开后的值**

展开为表达式的宏在使用处会显示该表达式的类型；若为常量表达式，还会一并显示其值

```case
tests/hover/macros/14_expansion_values
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 包含指令

`#include` 会显示它解析到的实际文件路径。

<!-- BEGIN GENERATED ITEMS: hover/directives -->

<!-- BEGIN CAPABILITY: supported -->

**包含指令**

悬停在 `#include` 上会显示它解析到的实际文件路径

```case
tests/hover/directives/01_include_paths
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档

无论在符号的何处悬停，声明上的文档注释都会显示在卡片中。注释按 Doxygen 的规则解析——包括其中的 Doxygen 命令和 Markdown 语法——代码补全、签名帮助和 `clice query` 也采用同样的解析规则。

<!-- BEGIN GENERATED ITEMS: hover/documentation -->

<!-- BEGIN CAPABILITY: supported -->

**文档注释**

声明上的 `///` 注释在声明处和各个使用处的卡片中都会显示

模板的使用处显示所选中模板特化的文档注释。

```case
tests/hover/documentation/01_doc_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**取值函数与设值函数**

未编写独立注释的简单取值函数（getter）或设值函数（setter），会自动生成访问对应字段的说明

```case
tests/hover/documentation/02_accessors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1320 -->

**@copydoc**

`@copydoc` 命令会显示它所指目标的文档

```case
tests/hover/documentation/03_copydoc
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2504 -->

**重写**

未编写独立注释的重写函数，会继承并显示被重写基类函数的文档

```case
tests/hover/documentation/04_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2506 -->

**重载**

未编写独立注释的重载函数不会显示文档，无论相邻的重载写了什么

不同的重载往往承担不同的职责，因此没有注释的重载不会借用前一个重载的注释。

```case
tests/hover/documentation/05_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1936 -->

**继承构造函数**

`using Base::Base` 中的名称会显示所继承的构造函数及其文档

```case
tests/hover/documentation/06_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#974 -->

**分节横幅注释**

与声明之间隔有空行的分节横幅注释不会当作该声明的文档

```case
tests/hover/documentation/07_banner_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义**

声明处的 `///` 注释在声明处和定义处都会显示

无论先在哪一处悬停，定义处的普通 `//` 注释都不会取代声明处的文档注释。

```case
tests/hover/documentation/08_declaration_and_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2057 -->

**Markdown 表格**

注释中的表格会渲染为表格

```case
tests/hover/documentation/09_markdown_tables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1040 -->

**缩进块**

注释中的缩进代码块会保留原有缩进及其上方的空行

```case
tests/hover/documentation/10_indented_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2148 -->

**游离注释**

与声明之间隔有空行的普通注释不会当作该声明的文档

`///` 或 `/** */` 注释即使与下方的声明隔着空行，仍按 Doxygen 的规则作为该声明的文档。

```case
tests/hover/documentation/11_stray_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2662 -->

**GTK-Doc 注释**

去掉 GTK-Doc 的符号名首行，并将每条 `@parameter:` 说明归入对应的形参旁显示

```case
tests/hover/documentation/12_gtk_doc
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**公式**

Doxygen 公式以行内代码形式展示，并去掉两侧的 `@f$` 标记

```case
tests/hover/documentation/13_latex_formulas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Doxygen 命令**

`@brief` 置于卡片正文的开头，每条 `@param`、`@return` 和 `@retval` 则显示在各自所描述的条目旁

```case
tests/hover/documentation/18_doxygen_commands
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**形参文档**

形参的卡片会显示所属函数的 `@param` 或所属模板的 `@tparam` 为它编写的说明

```case
tests/hover/documentation/20_parameter_docs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#782 -->

**宏文档**

宏的文档注释会显示在它的卡片中

```case
tests/hover/documentation/21_macro_docs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**行尾注释**

紧跟在字段、枚举项或变量同一行末尾的 `///<` 注释会作为它的文档；行尾的其他注释仅在上方没有文档注释时才作为它的文档

```case
tests/hover/documentation/22_trailing_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按原样显示的注释**

plain 格式按源码原样展示注释，保留其中的命令和标记

```case
tests/hover/documentation/23_plain_format
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**系统头文件**

系统头文件中的声明同样会显示其文档注释

```case
tests/hover/documentation/25_system_headers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块名会显示其接口单元中模块声明上方的注释及该接口单元所在的文件；来自模块的声明则会标明是否已导出，并显示模块接口为它编写的文档。

<!-- BEGIN GENERATED ITEMS: hover/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块名**

模块名无论出现在 `import` 中还是模块声明中，都会显示其接口单元中模块声明上方的注释及该单元所在的文件

```case
tests/hover/modules/01_module_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**导出的声明**

模块接口导出的声明会显示 `export`

```case
tests/hover/modules/02_export_status
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带文档的导出符号**

从模块导入的符号会显示其模块接口中编写的文档

```case
tests/hover/modules/04_documented_exports
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 属性

属性的卡片会显示它在代码中的写法及其含义。

<!-- BEGIN GENERATED ITEMS: hover/attributes -->

<!-- BEGIN CAPABILITY: supported clangd#1862 -->

**属性**

悬停在属性上会显示 clang 对它的说明，GNU 语法和标准语法均适用

```case
tests/hover/attributes/01_attributes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 无内容可显示

没有值得展示卡片的内容时，不弹出任何卡片。

<!-- BEGIN GENERATED ITEMS: hover/no_card -->

<!-- BEGIN CAPABILITY: supported -->

**无内容可显示**

内置基本类型、`const` 或 `virtual` 等关键字，以及空函数体内部均不显示卡片

```case
tests/hover/no_card/01_meaningless_tokens
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 展示格式

卡片采用 Markdown 格式；面对不支持 Markdown 的编辑器则退回纯文本。

<!-- BEGIN GENERATED ITEMS: hover/presentation -->

<!-- BEGIN CAPABILITY: supported -->

**纯文本卡片**

卡片默认采用 Markdown 格式，面对不支持 Markdown 的编辑器则输出纯文本

编辑器在启动时声明自己支持哪种格式，卡片随之调整输出。

```case
tests/hover/presentation/01_plain_text
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**从左到右阅读的类型**

C++ 由内向外书写的复杂类型改为从左到右排列：`*`、`&` 和 `[N]` 紧跟在所修饰的部分之后，函数写作 `(params) -> R`，成员指针写作 `C::-> T`

函数类型中嵌套的函数类型，或成员指针中嵌套的成员指针，会用一个希腊字母代称，并在下一行展开写出。

```case
tests/hover/presentation/03_inside_out_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**超长类型折行**

单行放不下的长类型会拆成多行书写，每行不超过 60 列

放不下的子类型用一个希腊字母代称，并在下方另起一行多缩进两列展开；重复出现的相同部分共用同一个字母。展开后仍放不下的实参或形参列表会继续折行，各行与左括号之后的位置对齐。

```case
tests/hover/presentation/04_long_type_lines
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**遵循文件的代码风格**

卡片中展示的代码——无论是声明还是宏的展开结果——均按文件上层目录中 `.clang-format` 配置的风格排版

未找到配置文件时，按 LLVM 风格排版，将 `*` 和 `&` 紧贴在类型一侧。

```case
tests/hover/presentation/05_configured_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
