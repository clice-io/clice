# 代码补全

为正在输入的内容提供候选项：名称、成员、调用、头文件和模块。

<!-- The cards in the sections below are generated from the documented cases
     in tests/code_completion/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 符号

输入时会补全当前作用域内的名称，写下 `ns::` 等限定符则会列出对应作用域中声明的名称。

<!-- BEGIN GENERATED ITEMS: code_completion/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**模糊非限定查找**

保留强前缀匹配项，过滤掉弱子序列匹配项

```case
tests/code_completion/symbols/01_unqualified_lookup
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板去重**

带有构造函数和推导指引（deduction guide）的类模板合并为单个补全项

```case
tests/code_completion/symbols/02_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏**

对象式宏按常量形式补全，函数式宏按带参数签名的函数形式补全

它们的实参与函数一样，按 `code_completion.arguments` 的设置补全。

```case
tests/code_completion/symbols/05_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间限定查找**

`ns::` 列出该命名空间自身的成员

```case
tests/code_completion/symbols/07_namespace_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**枚举成员**

有作用域枚举用 `Type::` 列出枚举项，无作用域枚举项直接按未限定名补全

```case
tests/code_completion/symbols/08_enum_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**必需的限定符**

仅凭未限定名无法访问的枚举项，会按未限定名匹配并在补全时自动带上所需的限定符

```case
tests/code_completion/symbols/11_required_qualifier
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文档**

补全项显示所指实体的文档注释；模板实例化的成员显示其所属模板编写的注释

```case
tests/code_completion/symbols/13_documentation
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他命名空间

其他命名空间声明的名称同样会补全，并带上访问它所需的限定符；当前文件尚未包含或导入的名称，还会自动插入所需的 `#include` 或 `import`。

<!-- BEGIN GENERATED ITEMS: code_completion/scopes -->

<!-- BEGIN CAPABILITY: supported -->

**其他命名空间中的名称**

其他命名空间声明的名称会带上访问它所需的限定符参与补全，排在当前作用域内的名称之后

```case
tests/code_completion/scopes/01_other_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尚未包含的头文件**

项目在当前文件尚未包含的头文件中声明的名称，补全时会同时插入引入它的 `#include`

```case
tests/code_completion/scopes/02_include_insertion
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尚未导入的模块**

项目中某个模块导出的名称，补全时会同时插入引入它的 `import`；当前模块自身的分区则按分区名导入

```case
tests/code_completion/scopes/03_import_insertion
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**已写出的作用域**

当前文件尚未包含的头文件中声明的名称，在写出前置命名空间后（如 `std::vec`）也会补全

无论当前文件此前是否见过该命名空间，补全时都会同时插入引入它的 `#include`。在命名空间别名、宏或类名之后，则只提供当前文件已可见的名称。

```case
tests/code_completion/scopes/20_written_qualifiers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**包含指令的定界符**

插入的 `#include` 按找到头文件的目录选用双引号或尖括号，配置另有指定时除外

位于当前文件同目录下或经 `-iquote`、`-I` 找到的头文件使用双引号；经 `-isystem`、`-idirafter` 找到的头文件使用尖括号。`angled_includes` 与 `quoted_includes` 则改为按头文件的路径选择，两者都匹配的头文件使用双引号。标准库的头文件始终使用尖括号。

```case
tests/code_completion/scopes/22_include_delimiters
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 成员访问

在 `.`、`->` 或 `::` 之后，补全对象或作用域的成员。

<!-- BEGIN GENERATED ITEMS: code_completion/member_access -->

<!-- BEGIN CAPABILITY: supported -->

**点号后的成员**

在点号后触发补全会列出对象所属类的成员

```case
tests/code_completion/member_access/01_struct_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域限定的成员**

在 `Class::` 之后，静态数据成员、嵌套类型、成员函数和注入类名（injected class name）都会列出

限定名补全不会过滤掉非静态上下文才能使用的成员：实例字段和析构函数会与静态成员及嵌套类型一同列出。

```case
tests/code_completion/member_access/04_scope_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承的成员**

派生类对象可补全自身成员以及从基类继承的成员

```case
tests/code_completion/member_access/05_inherited_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指定初始化器字段**

输入 `{ .` 会列出待初始化聚合类型的字段；若该聚合类型依赖模板参数，则列出所匹配偏特化的字段

```case
tests/code_completion/member_access/06_designated_initializer
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不可访问的成员**

私有成员和受保护成员在不可访问的位置不会出现在候选项中

```case
tests/code_completion/member_access/07_inaccessible_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类外定义**

在定义处的 `Class::` 之后会提供所有成员，包括私有成员

```case
tests/code_completion/member_access/10_out_of_line_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指针上的点号**

在指针后输入 `.` 时，会补全由 `->` 访问的成员，并自动把 `.` 替换为 `->`

```case
tests/code_completion/member_access/17_dot_on_pointer
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数初始化列表**

在构造函数的 `:` 之后，会列出尚未初始化的基类和成员，基类在前，按声明顺序排列

接下来该初始化的那一项排在最前，依据的是基类和成员的声明顺序，即使 C++ 会先于其他项初始化虚基类也是如此。每一项都填入其构造函数的参数；有多个构造函数的基类或成员只算一个补全项，除非设置为重载分开列出。此处不提供其他命名空间中的名称。

```case
tests/code_completion/member_access/22_constructor_initializers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 函数与代码片段

函数按调用形式补全，圆括号内填入 `code_completion.arguments` 指定的内容。以下所有选项均位于 `[code_completion]` 配置节中。

<!-- BEGIN GENERATED ITEMS: code_completion/functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**形参占位符代码片段**

函数调用按实参逐个插入 Tab 跳转点占位符；无参函数插入一对空括号

设为 `delimiters` 时，插入圆括号并将光标置于括号中间；设为 `none` 时，只插入函数名。

```case
tests/code_completion/functions_snippets/01_snippet_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名与返回类型**

形参列表和返回类型随函数名一起显示

```case
tests/code_completion/functions_snippets/02_function_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载合并**

重载集折叠为单个补全项，并注明重载数量

关闭合并后，每个重载单独作为一个补全项并显示各自的签名。

```case
tests/code_completion/functions_snippets/03_overload_bundle
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**调用括号**

补全函数调用时会附上圆括号，除非名称后已有实参或当前位置并非调用；非模板函数后面的 `<` 只是比较运算符，不会当作实参列表的开头

```case
tests/code_completion/functions_snippets/06_call_parentheses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板实参占位符**

类模板会为每个没有默认值的模板参数插入占位符；若所有形参都有默认值，则插入空尖括号

```case
tests/code_completion/functions_snippets/07_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句关键字**

语句关键字默认补全为关键字本身；开启对应选项后，会插入整条语句并为各个部分留出占位符

```case
tests/code_completion/functions_snippets/08_statement_snippets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Objective-C 方法**

消息从光标所在的那一段起补全选择器（selector），并为每个实参留出占位符；发往 `super` 的消息中，正在编写的方法会把自身形参原样传入。在实现块中声明方法时会补全整条声明；在点号之后，无参方法可按属性形式补全

补全列表展示选择器的第一段，其余各段作为签名显示。设为 `delimiters` 时，实参位置为空的 Tab 跳转点；设为 `none` 或实参已经写出时，只插入当前这一段。

```case
tests/code_completion/functions_snippets/12_objective_c_methods
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板与概念

在模板内部，即使成员依赖模板参数也能正常补全。

<!-- BEGIN GENERATED ITEMS: code_completion/templates -->

<!-- BEGIN CAPABILITY: supported -->

**依赖成员类型**

若变量的类型是依赖特化中的成员别名，可补全该别名所代表的类的成员

解析该别名时会代入代码写出的模板实参，因此 `Vec<Vec<T>>::value_type` 会列出 `Vec<T>` 的成员，而不是一片空白。代表引用类型的别名（如 `Vec<Vec<T>>::reference`）会列出被引用类的成员；对指向这类类型的指针使用 `->`，则会列出所指对象的成员。

```case
tests/code_completion/templates/01_dependent_member_type
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#443 -->

**依赖表达式的结果**

在模板内对下标运算或成员调用的返回值访问成员时，可补全其求值结果所属类的成员

对 `rows[0].`（作用在 `Vec<Vec<T>>` 上）触发补全会列出 `Vec<T>` 的成员，其类型按标准容器的声明方式顺着容器的 `reference` 别名解析得出。若成员函数有 `const` 重载，会根据对象的常量性选出所调用的重载（通过 const 对象访问的数据成员同样视为 const）；对返回的迭代器使用 `->` 则能访问元素本身。

```case
tests/code_completion/templates/05_dependent_expression_results
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导变量的成员**

由依赖初始化器初始化并声明为 `auto` 的变量，可补全其推导所得类的成员

`auto& row = rows[0]; row.` 会列出 `Vec<T>` 的成员。声明符按真实类型推导的规则生效：`const auto&` 使对象带 const 限定，按值的 `auto` 去掉初始化器的 const 限定，`auto&&` 和 `decltype(auto)` 保留该限定，`auto*` 则取指针所指类型。初始化自数据成员时，`decltype(auto)` 取该成员的声明类型；若加上圆括号，则取表达式本身的 const 引用类型。

```case
tests/code_completion/templates/07_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#493 clangd#1768 -->

**泛型 Lambda 形参**

Lambda 的 `auto` 形参可补全调用该 Lambda 时所传入类型的成员

调用既可以写在 Lambda 之后，也可以来自接收该 Lambda 的算法。以多种类型调用的 Lambda 会补全这些类型共有的成员名称。

```case
tests/code_completion/templates/14_generic_lambda_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1103 -->

**Concept 约束要求**

在受约束的模板内，输入点号后可补全 Concept 所要求的成员

Concept 中调用的成员会补全为成员函数，形参类型取自调用时传入的实参类型；在 `T::` 之后，Concept 指明的嵌套类型也能补全。

```case
tests/code_completion/templates/15_concept_requirements
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖特化中的成员**

类模板依赖特化的成员会代入代码写出的模板实参来显示类型

输入 `m.find(1)->`（对象类型为 `lib::map<int, T>`）后，`first` 的类型为 `const int`，`second` 为 `T`，而不是 pair 模板自身的 `_T1` 和 `_T2`。

```case
tests/code_completion/templates/29_library_specialization_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**多次实例化**

代码以多种类型调用的模板，会补全所有这些类型共有的名称

每个名称都会列出各个类型为它声明的重载。在 `if constexpr` 的分支内，只计入实际走入该分支的类型。写在模板前后的调用都会计入；超过八种类型时，由前八种决定。只要其中有一种类型不是类类型，就不存在共有名称，也不会提供任何补全项。

```case
tests/code_completion/templates/36_several_instantiations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 重写与类外定义

在派生类内部，基类的虚函数可补全为完整的函数声明。

<!-- BEGIN GENERATED ITEMS: code_completion/overrides -->

<!-- BEGIN CAPABILITY: supported -->

**重写声明**

在派生类内部，基类的虚函数会补全为完整的重写声明，包含返回类型和 `override`

在该重写函数的函数体内，该名称会补全为函数调用，而不会再补全出一条声明。

```case
tests/code_completion/overrides/01_override_declaration
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理指令

在行首的 `#` 之后会补全预处理指令，在 `#ifdef` 之后则会补全其上方已定义的宏。

<!-- BEGIN GENERATED ITEMS: code_completion/directives -->

<!-- BEGIN CAPABILITY: supported -->

**指令名**

在 `#` 之后会补全各类预处理指令，其中 `#elif`、`#else` 和 `#endif` 仅在条件编译块内提供；开启 `statement_snippets` 后，每种形式都会带上其参数的占位符

```case
tests/code_completion/directives/01_directive_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 包含路径

在 `#include` 之后会补全头文件名。已输入的名称优先匹配完整文件名，其次匹配前缀，再次匹配名称中的单词（如输入 `view` 可找到 `string_view`）；匹配程度相同时，双引号形式先列出包含方文件所在目录中的头文件，再列出各包含目录中的头文件，系统目录排在最后。外观上像头文件的文件排在其他文件之前：任意目录下带头文件扩展名的文件，以及系统目录等惯常存放无扩展名头文件位置下的无扩展名文件优先；源文件、纯文本文件，以及 clang 不允许直接包含的内置头文件（如 `<avxintrin.h>`）排在后面。framework 目录先提供其中的各个 framework，进入某个 framework 后再提供它的头文件。

<!-- BEGIN GENERATED ITEMS: code_completion/include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**双引号包含路径**

列出所配置搜索路径下的头文件和目录，并在目录末尾标出斜杠

```case
tests/code_completion/include_path_completion/01_include_quoted
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尖括号包含路径**

尖括号包含提供相同搜索路径下的候选项，并以 `>` 闭合

```case
tests/code_completion/include_path_completion/02_include_angled
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**闭合定界符**

接受头文件补全项后会自动闭合整条指令，替换光标后已有的内容而不会重复插入

原有的闭合定界符会被直接替换；若在路径较靠前的层级选中头文件，后面的剩余路径也会替换掉。含空格的名称会一直替换到闭合定界符为止；若无闭合定界符，则到第一个空格为止。

```case
tests/code_completion/include_path_completion/03_closing_delimiter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同目录下的头文件**

双引号包含无需配置任何搜索路径即可找到包含方文件同目录下的头文件

```case
tests/code_completion/include_path_completion/04_sibling_headers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**就近的头文件优先**

匹配程度相同的头文件名，依次先列出包含方文件所在目录、双引号包含目录、其余包含目录（按顺序）以及最后的系统目录中的结果；两个目录下同名的头文件只列出一次，取靠前的那个目录

```case
tests/code_completion/include_path_completion/09_header_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**库内部名称排后**

库的保留名称（如 `__algorithm/`、`_mingw.h`）排在所属搜索分组的其余名称之后，隐藏项（如 `.git/`）排在所有候选项最后

```case
tests/code_completion/include_path_completion/11_reserved_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Framework 头文件**

在 framework 目录（`-F`、`-iframework` 以及 macOS 上 SDK 的目录）中，各个 framework 先作为目录补全，进入后再补全其中的头文件：`<Foo/Bar.h>` 对应 `Foo.framework/Headers/Bar.h`

```case
tests/code_completion/include_path_completion/13_frameworks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**名称中的单词匹配**

已输入的名称先按完整名称、前缀、名称中单词的顺序匹配，这一优先级高于目录顺序：输入 `wsnap_view` 时，先列出系统目录中的 `wsnap_view`，再列出 `wsnap_viewport.h`，最后是 `old_wsnap_view.h`

```case
tests/code_completion/include_path_completion/14_name_words
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块导入

在 `import` 或 `export import` 之后会补全模块名。已输入的名称优先匹配完整名称，其次匹配前缀，再次匹配名称中的单词（如输入 `compat` 可找到 `app.compat`）；匹配程度相同时，先列出当前单元所属模块自身的分区，再按模块接口单元距当前文件的远近由近及远列出其他模块。

<!-- BEGIN GENERATED ITEMS: code_completion/modules -->

<!-- BEGIN CAPABILITY: supported -->

**导入语句**

在 `import` 之后补全已知的模块名，并自动插入结尾的分号

```case
tests/code_completion/modules/01_import_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前模块的分区**

在模块实现单元中，当前模块自身的分区以 `:partition` 形式补全，既不会提供当前模块自身，也不会提供其他模块的分区

原本已带有结尾分号的语句会保留该分号。

```case
tests/code_completion/modules/02_partition_imports
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**接口单元中的分区**

在模块接口单元中，只补全当前模块的接口分区

模块接口无法导出内部分区，且从内部分区导入的名称对导入该接口的外部单元也未必可见，因此内部分区只提供给当前模块的其他单元。

```case
tests/code_completion/modules/04_interface_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**就近的模块优先**

模块名补全优先列出接口单元距离当前文件最近的模块

```case
tests/code_completion/modules/09_nearest_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块文档**

`import` 之后提供的模块候选项会显示其接口单元中模块声明上方的文档注释

```case
tests/code_completion/modules/14_module_documentation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块名中的单词匹配**

已输入的名称先按完整模块名、前缀、名称中单词的顺序匹配，这一优先级高于接口单元的远近：输入 `compat` 时，先列出较远的 `compat`，再列出较近的 `compat_legacy` 和 `app.compat`

```case
tests/code_completion/modules/15_name_words
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 过滤

根据已输入内容与候选项各单词开头的匹配程度过滤候选项。

<!-- BEGIN GENERATED ITEMS: code_completion/filtering -->

<!-- BEGIN CAPABILITY: supported -->

**下划线过滤**

以下划线开头的内部符号默认隐藏，除非已输入的前缀本身也以下划线开头

```case
tests/code_completion/filtering/01_underscore_filter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**弃用标记**

带有 `[[deprecated]]` 的候选项会标上已弃用标记，并排在未弃用的同类候选项之后

```case
tests/code_completion/filtering/02_deprecated_tag
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词边界模糊匹配**

前缀 `fb` 匹配 `foo_bar_baz` 中各单词的开头

`frobnicate` 对 `fb` 只是零散的弱子序列匹配，因此被过滤掉；`foo_bar_baz` 在 `foo` 和 `bar` 的单词边界上命中，得以保留。

```case
tests/code_completion/filtering/03_fuzzy_word_boundary
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**前缀匹配优先于子序列匹配**

前缀完全匹配的候选项排在零散子序列匹配的候选项之前

输入前缀 `fo` 时，`format_output` 属于真正的前缀匹配，得分高于仅作为子序列匹配的 `fast_math_operation`。

```case
tests/code_completion/filtering/05_prefix_beats_subsequence
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词中间的补全**

在单词中间触发补全时会同时提供插入和替换两种范围：编辑器既可在单词剩余部分之前插入，也可替换整个单词

```case
tests/code_completion/filtering/06_inside_a_word
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 排序

在保留下来的候选项中，作用域离当前位置最近的名称排在前面，符合上下文期望类型的名称也排在前面。

<!-- BEGIN GENERATED ITEMS: code_completion/ranking -->

<!-- BEGIN CAPABILITY: supported -->

**最近作用域的名称优先**

前缀匹配的名称中，局部名称排在最前，其次是当前类的成员，再其次是文件内的名称，宏排在最后

```case
tests/code_completion/ranking/01_closest_first
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**期望类型优先**

类型符合上下文期望的名称，排在前缀同样匹配的其他名称之前

```case
tests/code_completion/ranking/02_expected_type
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 触发字符

已注册：`. < > : " / #` 和空格。

| 字符 | 上下文       | 行为                                  |
| ---- | ------------ | ------------------------------------- |
| `.`  | 成员访问     | 语义补全                              |
| `>`  | 构成 `->`    | 指针成员补全；其他位置的 `>` 忽略     |
| `:`  | 构成 `::`    | 作用域补全                            |
| `<`  | `#include <` | 包含路径补全                          |
| `"`  | `#include "` | 包含路径补全                          |
| `/`  | 路径分隔符   | 继续补全包含路径                      |
| ` `  | `import` 后  | 模块名补全（由编辑器扩展控制）        |
| `#`  | 行首         | 预处理指令名补全；其他位置的 `#` 忽略 |
