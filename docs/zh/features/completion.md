# 代码补全

为正在输入的内容提供候选项：名称、成员、调用、头文件和模块。

<!-- The cards in the sections below are generated from the documented cases
     in tests/code_completion/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 符号

输入时会补全当前作用域内的名称，使用 `ns::` 等限定符则会列出对应作用域中声明的内容。

<!-- BEGIN GENERATED ITEMS: code_completion/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**模糊非限定查找**

保留匹配度高的前缀匹配项，排除匹配度低的子序列匹配项

```case
tests/code_completion/symbols/01_unqualified_lookup
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板去重**

带有构造函数和推导指引（deduction guide）的类模板仍只显示为一个补全项

```case
tests/code_completion/symbols/02_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏**

对象式宏以常量形式补全，函数式宏以带参数签名的函数形式补全

其实参的插入方式与函数相同，由 `code_completion.arguments` 控制。

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

有作用域枚举通过 `Type::` 列出枚举项，无作用域枚举的枚举项则通过其未限定名称补全

```case
tests/code_completion/symbols/08_enum_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**必需的限定符**

无法通过未限定名称查找到的枚举项，会按其未限定名称匹配，并在补全时加上所需的限定符

```case
tests/code_completion/symbols/11_required_qualifier
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文档**

补全项显示其所指实体的文档注释；模板实例化中的成员显示模板为它编写的注释

```case
tests/code_completion/symbols/13_documentation
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他命名空间

其他命名空间中声明的名称同样会补全，并带上访问它所需的限定符；若当前文件尚未包含或导入声明它的头文件或模块，补全时还会加上所需的 `#include` 或 `import`。

<!-- BEGIN GENERATED ITEMS: code_completion/scopes -->

<!-- BEGIN CAPABILITY: supported -->

**其他命名空间中的名称**

其他命名空间中声明的名称会带上访问它所需的限定符补全，排在作用域内的名称之后

```case
tests/code_completion/scopes/01_other_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尚未包含的头文件**

项目中的名称若声明在当前文件尚未包含的头文件里，补全时会一并插入引入它的 `#include`

```case
tests/code_completion/scopes/02_include_insertion
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尚未导入的模块**

项目中某个模块导出的名称，补全时会一并插入引入它的 `import`；当前模块自身的分区则以分区名导入

```case
tests/code_completion/scopes/03_import_insertion
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 成员访问

在 `.`、`->` 或 `::` 之后，会补全对象或作用域的成员。

<!-- BEGIN GENERATED ITEMS: code_completion/member_access -->

<!-- BEGIN CAPABILITY: supported -->

**点号后的成员**

在点号后补全时，会列出对象所属类的成员

```case
tests/code_completion/member_access/01_struct_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域限定的成员**

在 `Class::` 后会列出静态数据成员、嵌套类型、方法和注入类名（injected class name）

限定名补全不会只列出静态上下文中可用的成员：实例字段和析构函数也会与静态成员、嵌套类型一同显示。

```case
tests/code_completion/member_access/04_scope_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承的成员**

对派生类对象进行补全时，列出其自身及基类的成员

```case
tests/code_completion/member_access/05_inherited_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指定初始化器的字段**

输入 `{ .` 会列出正在初始化的聚合体的字段；若其类型依赖于模板参数，则列出匹配的偏特化中的字段

```case
tests/code_completion/member_access/06_designated_initializer
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不可访问的成员**

私有和受保护的成员不会出现在无法使用它们的位置

```case
tests/code_completion/member_access/07_inaccessible_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类外定义**

在定义中输入 `Class::` 后，会列出所有成员，包括私有成员

```case
tests/code_completion/member_access/10_out_of_line_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指针后的点号**

在指针后输入 `.` 时，会补全通过 `->` 可访问的成员，并把 `.` 改为 `->`

```case
tests/code_completion/member_access/17_dot_on_pointer
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 函数与代码片段

函数会补全为调用形式，括号内插入什么由 `code_completion.arguments` 决定。以下所有选项均位于 `[code_completion]` 配置节中。

<!-- BEGIN GENERATED ITEMS: code_completion/functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**参数占位符代码片段**

函数调用补全会为每个实参插入可用 Tab 键依次跳转的占位符；无参函数则插入空括号

设为 `delimiters` 时，调用会插入括号并将光标置于括号内；设为 `none` 时，只插入名称。

```case
tests/code_completion/functions_snippets/01_snippet_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名与返回类型**

参数列表和返回类型随函数名一起显示

```case
tests/code_completion/functions_snippets/02_function_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载合并**

一组重载合并为一个条目，并显示重载数量

关闭重载合并后，每个重载都单独列为一个条目，并显示各自的签名。

```case
tests/code_completion/functions_snippets/03_overload_bundle
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**调用括号**

补全时会为调用添加括号，除非名称后已有实参，或该名称并非用于调用

```case
tests/code_completion/functions_snippets/06_call_parentheses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板实参占位符**

补全类模板时，为每个没有默认值的模板形参插入一个占位符；若所有形参都有默认值，则插入空尖括号

```case
tests/code_completion/functions_snippets/07_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句关键字**

语句关键字补全为关键字；启用该选项后，会插入整条语句，并为每个部分添加占位符

```case
tests/code_completion/functions_snippets/08_statement_snippets
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板与概念

在模板内部，即使成员依赖于模板形参，也能进行补全。

<!-- BEGIN GENERATED ITEMS: code_completion/templates -->

<!-- BEGIN CAPABILITY: supported -->

**依赖成员类型**

若变量的类型是依赖于模板形参的特化中的成员类型别名，就能补全该别名所指类的成员

解析别名时，会代入代码中写出的模板实参，因此 `Vec<Vec<T>>::value_type` 会列出 `Vec<T>` 的成员，而不是不显示任何内容。如果别名表示引用（`Vec<Vec<T>>::reference`），则会列出所引用类的成员；对指向这类对象的指针使用 `->`，则会列出所指对象的成员。

```case
tests/code_completion/templates/01_dependent_member_type
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#443 -->

**依赖表达式的结果**

在模板中对下标或成员调用的返回值进行成员访问时，补全该返回值所属类的成员

对于 `Vec<Vec<T>>`，`rows[0].` 会按照标准容器的声明方式，通过容器的 `reference` 别名解析出类型，并列出 `Vec<T>` 的成员。如果成员函数有 `const` 重载，则根据对象是否为 const 来选择调用哪个重载；通过 const 对象访问的数据成员也视为 const，对返回的迭代器使用 `->` 则可访问元素。

```case
tests/code_completion/templates/05_dependent_expression_results
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导变量的成员**

用依赖初始化器声明为 `auto` 的变量，补全其推导出的类的成员

`auto& row = rows[0]; row.` 会列出 `Vec<T>` 的成员。声明符按实际类型推导的规则生效：`const auto&` 使对象具有 const 属性，按值声明的 `auto` 去除初始化器的 const 属性，`auto&&` 和 `decltype(auto)` 保留该属性，而 `auto*` 则取指针所指对象。对于数据成员，`decltype(auto)` 取该成员的声明类型；如果加上括号，则取表达式的 const 引用类型。

```case
tests/code_completion/templates/07_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#493 clangd#1768 -->

**泛型 Lambda 参数**

Lambda 的 `auto` 参数会补全调用该 Lambda 时所用类型的成员

调用可以写在 Lambda 之后，也可以是接收该 Lambda 的算法。以多种类型调用的 Lambda 不提供补全。

```case
tests/code_completion/templates/14_generic_lambda_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1103 -->

**Concept 要求**

在受约束的模板内，输入点号后可以补全 Concept 所要求的成员

概念中调用的成员会补全为方法，参数类型取自调用时传入的实参；在 `T::` 之后，它命名的嵌套类型也会补全。

```case
tests/code_completion/templates/15_concept_requirements
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过依赖特化访问的成员**

类模板依赖特化的成员，按代码写出的模板实参显示

对于 `lib::map<int, T>`，输入 `m.find(1)->` 后，`first` 为 `const int`，`second` 为 `T`，而不是 pair 自身的 `_T1` 和 `_T2`。

```case
tests/code_completion/templates/29_library_specialization_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 重写与类外定义

在派生类中，基类的虚函数会补全为完整声明。

<!-- BEGIN GENERATED ITEMS: code_completion/overrides -->

<!-- BEGIN CAPABILITY: supported -->

**重写声明**

在派生类中，基类的虚函数会补全为完整的重写声明，包含返回类型和 `override`

在重写函数内部，函数名会补全为调用，而不会再补全出一个声明。

```case
tests/code_completion/overrides/01_override_declaration
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理指令

在行首的 `#` 之后会补全预处理指令，在 `#ifdef` 之后会补全它上方已定义的宏。

<!-- BEGIN GENERATED ITEMS: code_completion/directives -->

<!-- BEGIN CAPABILITY: supported -->

**指令名**

在 `#` 之后会补全预处理指令，其中 `#elif`、`#else` 和 `#endif` 只在条件编译块内提供；启用 `statement_snippets` 后，每种写法都带有其参数的占位符

```case
tests/code_completion/directives/01_directive_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 包含路径

在 `#include` 之后会补全头文件名：引号形式先搜索包含方文件自身所在的目录，再搜索包含目录，系统目录排在最后。只有看起来像头文件的文件才会成为候选：任何目录下带头文件扩展名的文件，以及系统目录等存放此类头文件的位置中不带扩展名的文件。

<!-- BEGIN GENERATED ITEMS: code_completion/include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**引号包含路径**

补全会列出已配置的搜索路径中的头文件和目录，并用末尾的斜杠标识目录

```case
tests/code_completion/include_path_completion/01_include_quoted
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尖括号包含路径**

使用尖括号包含头文件时，补全会提供相同搜索路径中的候选项，并以 `>` 闭合

```case
tests/code_completion/include_path_completion/02_include_angled
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**闭合定界符**

接受头文件补全项后，会闭合指令，并替换光标后已输入的内容，避免重复插入

补全会替换已有的定界符；如果在路径中较靠前的部分选择头文件，还会替换路径的剩余部分。

```case
tests/code_completion/include_path_completion/03_closing_delimiter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同目录下的头文件**

使用引号包含头文件时，无需配置搜索路径即可找到与当前文件同目录的头文件

```case
tests/code_completion/include_path_completion/04_sibling_headers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**距离最近的头文件优先**

头文件名先列出包含方文件所在目录中的头文件，再列出引号包含目录中的头文件，然后按顺序列出其他包含目录中的头文件，系统目录排在最后；两个目录都有的头文件名只列出一次，取自靠前的目录

```case
tests/code_completion/include_path_completion/09_header_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**库内部名称排在最后**

库的保留名称（`__algorithm/`、`_mingw.h`）排在同一搜索组的其他名称之后，隐藏名称（`.git/`）排在所有其他名称之后

```case
tests/code_completion/include_path_completion/11_reserved_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块导入

在 `import` 或 `export import` 之后会补全模块名：先列出当前单元所属模块自身的分区，再列出接口单元离当前文件最近的模块。

<!-- BEGIN GENERATED ITEMS: code_completion/modules -->

<!-- BEGIN CAPABILITY: supported -->

**导入语句**

在 `import` 后补全已知的模块名，并插入结尾的分号

```case
tests/code_completion/modules/01_import_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前模块的分区**

在实现单元中，当前模块自身的分区以 `:partition` 的形式补全，而模块本身和其他模块的分区都不会出现在候选项中

已以分号结尾的语句会保留该分号。

```case
tests/code_completion/modules/02_partition_imports
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**接口单元中的分区**

在接口单元中，只会补全模块的接口分区

接口不能导出内部分区，它从内部分区导入的名称对该接口的导入方也未必可达，因此只有模块的其他单元才会补全内部分区。

```case
tests/code_completion/modules/04_interface_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**距离最近的模块优先**

模块名补全会优先列出接口单元离当前文件最近的模块

```case
tests/code_completion/modules/09_nearest_modules
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 过滤

根据输入内容与候选项中各单词开头的匹配情况筛选候选项。

<!-- BEGIN GENERATED ITEMS: code_completion/filtering -->

<!-- BEGIN CAPABILITY: supported -->

**下划线过滤**

以下划线开头的内部符号默认隐藏，只有输入的前缀也以下划线开头时才会显示

```case
tests/code_completion/filtering/01_underscore_filter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**弃用标记**

带有 `[[deprecated]]` 的候选项会标记为已弃用，并排在与之对应的不带该属性的候选项之后

```case
tests/code_completion/filtering/02_deprecated_tag
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词边界模糊匹配**

前缀 `fb` 匹配 `foo_bar_baz` 中各单词的开头

对于 `fb`，`frobnicate` 只能形成较弱的分散子序列匹配，因此被过滤掉；`foo_bar_baz` 在 `foo` 和 `bar` 的单词边界上匹配，因此得以保留。

```case
tests/code_completion/filtering/03_fuzzy_word_boundary
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**前缀匹配优先于子序列匹配**

精确匹配前缀的候选项排在分散子序列匹配的候选项之前

对于前缀 `fo`，`format_output` 属于真正的前缀匹配，得分高于只能按子序列匹配的 `fast_math_operation`。

```case
tests/code_completion/filtering/05_prefix_beats_subsequence
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词中间的补全**

在单词中间补全时，会提供两种编辑范围：编辑器可以在单词剩余部分之前插入补全内容，也可以替换整个单词

```case
tests/code_completion/filtering/06_inside_a_word
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 排序

在过滤后剩下的候选项中，离代码最近的名称排在前面，代码所期望类型的名称也排在前面。

<!-- BEGIN GENERATED ITEMS: code_completion/ranking -->

<!-- BEGIN CAPABILITY: supported -->

**距离最近的名称优先**

在前缀匹配的名称中，局部符号排在最前，其次是类的成员，然后是文件中的名称，宏排在最后

```case
tests/code_completion/ranking/01_closest_first
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**期望类型优先**

代码所期望类型的名称，排在前缀同样匹配的其他名称之前

```case
tests/code_completion/ranking/02_expected_type
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 触发字符

已注册：`. < > : " /` 和空格。

| 字符 | 上下文         | 行为                                |
| ---- | -------------- | ----------------------------------- |
| `.`  | 成员访问       | 语义补全                            |
| `>`  | 通过 `->` 触发 | 指针成员补全；其他位置的 `>` 不触发 |
| `:`  | 通过 `::` 触发 | 作用域补全                          |
| `<`  | `#include <`   | 包含路径补全                        |
| `"`  | `#include "`   | 包含路径补全                        |
| `/`  | 路径分隔符     | 继续补全包含路径                    |
| ` `  | `import` 之后  | 模块名补全（受扩展限制）            |
