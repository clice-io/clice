# 代码导航

## 跳转到定义

“跳转到定义”会从名称跳转到其所指实体的定义，并从定义跳回声明。

<!-- BEGIN GENERATED ITEMS: navigation/definition -->

<!-- BEGIN CAPABILITY: supported -->

**只有声明的符号**

如果找不到符号的定义，则跳转到其声明

```case
tests/navigation/definition/01_declaration_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区导入**

模块分区导出的名称会跳转到该分区，导入语句中的分区名也会跳转到该分区

```case
tests/navigation/definition/02_partition_import
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义交替跳转**

从使用处跳转到定义，从声明处或定义处跳转到另一处

如果函数的声明和定义分开，在声明处或定义处发起的每次请求都会跳转到另一处。如果函数没有单独的声明，在任何位置发起请求都会跳转到其定义。

```case
tests/navigation/definition/03_declaration_and_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**在 `#include` 指令上跳转到定义**

在包含指令上执行跳转到定义，会打开其引用的文件

文件开头的包含指令与文件后面普通的包含指令行为一致。

```case
tests/navigation/definition/05_include_directives
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**局部符号导航**

在局部变量或参数上执行“跳转到定义”，会跳转到其在函数体内的声明

```case
tests/navigation/definition/06_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**无法解析的变量类型**

如果变量的类型无法解析，目前还无法从其使用处跳转到该变量的声明

虽然变量的声明就在那里，但在该变量后续的使用处执行“跳转到定义”不会返回任何结果。

```case
tests/navigation/definition/07_error_recovery
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到声明

从符号的使用处或定义处跳转到其声明。在 C++ 中，许多实体的声明和定义是分开的。

clice 返回声明和定义的位置，并排除光标当前所在的位置；内联定义的符号没有单独的声明。因此，声明与定义位置会像“跳转到定义”一样交替跳转。

<!-- BEGIN GENERATED ITEMS: navigation/declaration -->

<!-- BEGIN CAPABILITY: supported -->

**函数**

从使用处和单独给出的定义处跳转到函数原型

从调用处和单独给出的定义处执行“跳转到声明”，都能到达函数原型；原型会与这两处非当前光标位置交替跳转。

```case
tests/navigation/declaration/01_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**前置声明的记录类型**

对于先前置声明、后定义的类，从使用处跳转时会同时列出两者；前置声明仍保留在声明集合中，不会因优先返回定义而被丢弃

```case
tests/navigation/declaration/02_forward_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**静态数据成员**

从静态成员的使用处跳转到类内声明

静态数据成员在类内声明、类外定义；在使用处执行“跳转到声明”时，会同时列出类内声明和定义。

```case
tests/navigation/declaration/03_static_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`extern` 变量**

从外部变量的使用处跳转到其声明

从 `extern` 变量的使用处跳转时，会同时列出 `extern` 声明和定义性声明，因此始终可以从使用处到达头文件中的声明。

```case
tests/navigation/declaration/04_extern_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**多处声明**

从使用处跳转到所有声明位置

当函数在多处声明时，从使用处执行“跳转到声明”会列出所有声明，而不只是最近的一处。

```case
tests/navigation/declaration/05_redeclarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名的表面差异**

参数名以及参数上的顶层 `const` 都不属于函数类型：下面的声明和定义以不同的写法表示同一个函数，但“跳转到声明”仍能从使用处跳转到函数原型

```case
tests/navigation/declaration/06_signature_spelling
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到类型定义

跳转到符号的类型定义。适用于变量、参数、字段以及其他具有类型的命名实体。当类型是类型别名或类似指针的包装类型时，应解开别名或包装，跳转到其底层类型或所指向类型的定义。

<!-- BEGIN GENERATED ITEMS: navigation/type_definition -->

<!-- BEGIN CAPABILITY: supported -->

**变量和参数**

在局部变量或参数上执行“跳转到类型定义”，会跳转到其类型的定义

```case
tests/navigation/type_definition/01_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类和结构体字段**

在字段访问处执行“跳转到类型定义”，会跳转到该字段类型的定义

```case
tests/navigation/type_definition/02_fields
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由 `auto` 推导类型的变量**

在以 `auto` 声明的变量上执行“跳转到类型定义”，会跳转到其推导出的类型；在 `auto` 关键字上执行该操作也是如此

```case
tests/navigation/type_definition/03_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1026 -->

**跳转到智能指针所指向的类型**

在智能指针变量上执行“跳转到类型定义”，会跳转到包装类型本身；不支持跳过包装类型并跳转到其所指向的类型

```case
tests/navigation/type_definition/04_smart_pointers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**类型别名**

在使用类型别名的变量上执行“跳转到类型定义”，会跳转到 `using` 或 `typedef` 声明；尚不能解析别名并跳转到底层类型的定义

```case
tests/navigation/type_definition/05_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）变量**

在结构化绑定上执行“跳转到类型定义”，会跳转到所绑定成员的类型定义

```case
tests/navigation/type_definition/06_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指针、引用和数组**

“跳转到类型定义”会透过指针、引用和数组，跳转到元素类型的定义

```case
tests/navigation/type_definition/07_pointers_and_references
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到实现

在虚方法上执行“跳转到实现”，会列出它的重写方法；在类上执行，则会列出其派生类。

<!-- BEGIN GENERATED ITEMS: navigation/implementation -->

<!-- BEGIN CAPABILITY: supported -->

**重写链**

沿重写链逐级跳转到实现

在三级重写链中，从每个方法执行“跳转到实现”都会到达下一级的重写方法：从基类到中间类，再从中间类到叶子类。

```case
tests/navigation/implementation/01_override_chain
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同级重写**

跳转到实现时列出所有同级重写方法

在虚方法上执行“跳转到实现”，会列出各个同级派生类中的所有重写方法。

```case
tests/navigation/implementation/02_sibling_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#854 -->

**非虚函数**

尚不能从非虚函数声明跳转到单独给出的定义

在非虚函数声明上执行“跳转到实现”，无法到达其单独给出的定义，会返回空结果。

```case
tests/navigation/implementation/03_non_virtual
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基类**

从基类跳转到所有派生类

在基类名称上执行“跳转到实现”，会列出从该类派生的类。

```case
tests/navigation/implementation/04_derived_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板鸭子类型导航**

在依赖调用上执行“跳转到实现”，会到达该调用在模板唯一实例化中解析到的方法

函数模板和泛型 Lambda 均适用。

```case
tests/navigation/implementation/05_dependent_calls
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 查找引用

“查找引用”会列出所有使用该符号名称的位置，包括其声明。

<!-- BEGIN GENERATED ITEMS: navigation/references -->

<!-- BEGIN CAPABILITY: supported -->

**引用结果中的声明**

引用查询会返回符号的声明、单独给出的定义以及所有使用位置，因此从符号的任意一个出现位置都能跳转到它的所有相关位置

```case
tests/navigation/references/01_declarations_included
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1081 -->

**范围 for 循环引用**

在 `begin` 上执行“查找引用”，只会返回它自身的声明；隐式调用它的范围 for 循环不会列入引用结果

```case
tests/navigation/references/02_range_for
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**隐式构造函数和析构函数调用**

在构造函数上执行“查找引用”，只会返回显式使用它的位置；隐式调用该构造函数或对应析构函数的对象定义不会包含在结果中

```case
tests/navigation/references/03_implicit_construction
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 clangd#1872 -->

**通过转发函数产生的引用**

在构造函数上执行“查找引用”，不会包含通过完美转发工厂间接调用它的位置

```case
tests/navigation/references/04_forwarding_factories
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#258 clangd#675 -->

**模板中的依赖引用**

在成员上执行“查找引用”，结果会包含模板中的依赖调用位置，前提是该模板只用该成员所属的类实例化

```case
tests/navigation/references/05_dependent_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2139 -->

**引用的读写分类**

引用查询的响应只包含位置信息，因此查看结果时无法区分写入和读取；不支持为每条结果标注访问类型

```case
tests/navigation/references/06_access_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#177 -->

**引用的上下文**

每条引用只提供位置信息，不附带其所在函数的名称，因此结果中除文件和行号外没有其他上下文

```case
tests/navigation/references/07_enclosing_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签和 goto 引用**

在标签上执行“查找引用”，会列出标签本身以及所有跳转到该标签的 `goto`

```case
tests/navigation/references/08_labels
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

在 `auto` 和 `decltype` 上执行导航操作，会跳转到它们所代表的类型。

<!-- BEGIN GENERATED ITEMS: navigation/deduced_types -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 推导类型导航**

对 `auto` 执行“跳转到定义”会跳转到推导出的类型，就像该位置直接写出了类型一样

```case
tests/navigation/deduced_types/01_auto_keyword
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2055 -->

**指针和引用形式的 `auto`**

跳转时会剥去推导类型外层的指针、引用和数组：`auto*`、`const auto&`、`auto&&`、`decltype(auto)` 以及推导为指针的 `auto` 都会跳转到类

结构化绑定（structured bindings）的 `auto` 跳转到被分解对象的类型。

```case
tests/navigation/deduced_types/02_auto_declarators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` 类型导航**

对 `decltype` 执行“跳转到定义”会跳转到其操作数所指的类型；如果该类型本身也是用 `decltype` 声明的，则会逐层追溯

```case
tests/navigation/deduced_types/03_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导出的模板和别名**

当 `auto` 推导为模板特化时，会跳转到模板，或所选的显式特化或偏特化；通过别名推导时，则跳转到该别名

内置类型和 Lambda 的闭包类型没有可跳转的声明，`new` 表达式中的 `auto` 也不会跳转到任何位置。展开为 `auto` 的宏则跳转到该宏本身。

```case
tests/navigation/deduced_types/04_deduced_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 参数和返回类型**

简写函数模板的参数中的 `auto` 会跳转到其唯一实例化所用的类型；返回类型中的 `auto` 则跳转到返回的类型

如果模板以多种类型实例化，参数中的 `auto` 不会跳转到其中任何一个。尾置返回类型（trailing return type）开头的 `auto` 跳转到箭头之后的类型。

```case
tests/navigation/deduced_types/05_parameters_and_returns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板中的 `auto`**

在模板内部，若 `auto` 的初始化器依赖于模板参数，则会跳转到该初始化器解析出的类型

模板只实例化一次时，模板参数代表其实参；以多种类型实例化时，模板参数代表它自身。

```case
tests/navigation/deduced_types/06_dependent_auto
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板

在模板内部，依赖于模板参数的名称仍可跳转到能够解析到的声明。

<!-- BEGIN GENERATED ITEMS: navigation/templates -->

<!-- BEGIN CAPABILITY: supported -->

**待决成员导航**

在从未实例化的模板中，访问待决类型（dependent type）对象的成员时，会解析到相应类模板中声明的成员

```case
tests/navigation/templates/01_dependent_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#212 -->

**模板特化导航**

对显式特化的名称执行“跳转到定义”会跳转到该特化本身；不支持从该特化继续跳转到它所特化的主模板

```case
tests/navigation/templates/02_explicit_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖调用的重载候选**

依赖调用可能命中多个重载时，会逐一列出这些重载

每个候选各自给出结果：有定义的跳转到定义，没有定义的跳转到声明。

```case
tests/navigation/templates/03_overload_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖表达式的成员**

在依赖下标、调用或 `auto` 变量的结果上访问成员时，解析到类模板中声明的成员

调用存在 `const` 重载时，由对象是否为 const 决定解析到哪一个重载；参数不同的各个重载则全部列出。

```case
tests/navigation/templates/04_dependent_expressions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

宏实参中写出的名称或由宏生成的名称，都能像其他名称一样跳转；宏的引用包括所有展开它或检查它的位置。

<!-- BEGIN GENERATED ITEMS: navigation/macros -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参中的名称**

宏实参中的名称可以像宏外的名称一样跳转

在名称的书写位置，跳转会在定义和声明之间切换；后续使用该名称时，则会跳转到宏所声明的函数。宏自身的名称无论出现在定义处还是调用处，都会解析到该宏。

```case
tests/navigation/macros/01_macro_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏生成的名称**

宏生成的名称会解析到生成该名称的宏调用

从通过 Token 拼接生成或写在宏体中的名称的使用处导航，会跳转到宏调用处；调用处的宏名称则解析到宏本身。宏体内的 Token 只有经过展开才有具体含义，因此对其导航不会返回结果。

```case
tests/navigation/macros/02_generated_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏引用**

宏引用搜索包括宏展开、条件测试和取消定义

同一名称的每次 `#define` 都被视为独立的符号，因此在 `#undef` 之后重新定义的宏只会收集自身的使用位置。

```case
tests/navigation/macros/04_macro_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#346 -->

**嵌套宏引用**

查找宏引用时，不会包含其他宏定义的宏体中对该宏的引用

```case
tests/navigation/macros/05_nested_macros
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 隐式目标

跳转到隐式调用的代码定义。在 C++ 中，许多语法结构会隐式调用构造函数、运算符、转换函数等。从语法结构（花括号、关键字、运算符 Token）跳转到实际调用的函数，对于理解究竟执行了哪些代码至关重要。

隐式导航要求源码中的 Token 没有歧义。如果 Token 已有明确的“跳转到定义”目标（例如，变量名始终跳转到其声明），就不能再用它来导航到隐式调用。

<!-- BEGIN GENERATED ITEMS: navigation/implicit_targets -->

<!-- BEGIN CAPABILITY: unsupported -->

**`override` / `final`**

`override` 和 `final` 尚不能跳转到被重写的基类方法

在 `override` 或 `final` 说明符上执行“跳转到定义”，无法跳转到被重写的基类虚方法。

```case
tests/navigation/implicit_targets/01_override_and_final
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**`break` / `continue`**

`break` 和 `continue` 尚不能跳转到它们所在的控制语句

在 `break` 或 `continue` 上执行“跳转到定义”，无法跳转到其控制的循环或 switch 语句的头部。

```case
tests/navigation/implicit_targets/02_break_and_continue
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`delete` 表达式**

`delete` 尚不能跳转到所调用的析构函数

在 `delete` 上执行“跳转到定义”，无法跳转到它调用的析构函数。

```case
tests/navigation/implicit_targets/03_delete_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`new` 表达式**

`new` 可跳转到重载的内存分配函数，但不能跳转到构造函数

在 `new` 上执行“跳转到定义”，可跳转到类中重载的 `operator new`。返回结果不包含同一表达式调用的构造函数。

```case
tests/navigation/implicit_targets/04_new_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载运算符**

重载运算符的 Token 可跳转到其定义

在重载运算符的 Token 上执行“跳转到定义”，可跳转到该运算符的定义。二元、下标、调用和箭头运算符（`+`、`[]`、`()`、`->`）均可解析。

```case
tests/navigation/implicit_targets/05_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C++20 重写运算符**

重写后的比较可跳转到实际实现该比较的运算符

对于按 C++20 重写规则合成的比较，在源码中写出的运算符上执行“跳转到定义”，可跳转到实际实现该比较的运算符：`!=` 跳转到 `operator==`，`>` 跳转到 `operator<=>`。

```case
tests/navigation/implicit_targets/06_rewritten_comparisons
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**用户定义字面量（user-defined literals）**

字面量后缀可跳转到其字面量运算符

在用户定义字面量的后缀上执行“跳转到定义”，可跳转到它调用的 `operator""`。

```case
tests/navigation/implicit_targets/07_user_defined_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1931 -->

**隐式转换运算符**

转换上下文尚不能跳转到所调用的转换运算符

在触发用户定义转换的上下文（条件判断或 `!`）中执行“跳转到定义”，无法跳转到该转换运算符。

```case
tests/navigation/implicit_targets/08_implicit_conversions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**强制类型转换导航**

调用构造函数的强制类型转换可跳转到所选的构造函数

调用用户定义转换运算符的 `static_cast` 尚不能跳转到该运算符。

```case
tests/navigation/implicit_targets/09_casts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**范围 for 循环（range-based for）**

范围 for 循环中的冒号尚不能跳转到 `begin()` 或 `end()`

在范围 for 循环的 `:` 上执行“跳转到定义”，无法跳转到为该范围选用的 `begin()` 或 `end()`。

```case
tests/navigation/implicit_targets/10_range_for
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`co_await` / `co_yield` / `co_return`**

`co_yield` 可跳转到其 promise 对象的方法，而其他协程关键字不能

在 `co_yield` 上执行“跳转到定义”，可跳转到 promise 对象的 `yield_value`。`co_await` 和 `co_return` 关键字尚不能跳转到等待器（awaiter）或 promise 对象的方法。

```case
tests/navigation/implicit_targets/11_coroutine_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 隐式构造

从初始化、返回、捕获和分解语法执行导航，可跳转到隐式选定的构造函数、聚合类型定义或绑定。

<!-- BEGIN GENERATED ITEMS: navigation/implicit_construction -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数调用**

圆括号和花括号可跳转到所选的构造函数

在构造函数调用的左圆括号或左花括号上执行“跳转到定义”，可跳转到重载决议选中的构造函数，适用于 `T(args)` 和 `T{args}` 两种形式。

```case
tests/navigation/implicit_construction/01_constructor_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**拷贝／移动构造与赋值**

赋值中的 `=` 可跳转到赋值运算符，而拷贝初始化和移动初始化则不能

在赋值中的 `=` 上执行“跳转到定义”，可跳转到赋值运算符。引入拷贝初始化或移动初始化的 `=`（`T b = a;`）属于初始化语法，而非运算符调用，目前尚无法解析。

```case
tests/navigation/implicit_construction/02_copy_and_move
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**CTAD**

使用 CTAD 的构造调用可跳转到推导出的特化的构造函数

当类模板实参推导选中特化时，在构造函数调用上执行“跳转到定义”，可跳转到选中的构造函数，而不只是类模板。

```case
tests/navigation/implicit_construction/03_deduced_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**聚合初始化**

聚合初始化的花括号可跳转到聚合类型的定义

聚合类型没有构造函数，因此在其初始化器的花括号上执行“跳转到定义”，会跳转到聚合类型的定义。

```case
tests/navigation/implicit_construction/04_aggregates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员初始化列表**

成员初始化器的圆括号可跳转到选中的基类或成员构造函数

在初始化列表中各初始化器的左圆括号上，可跳转到该初始化器调用的基类或成员构造函数。初始化器名称本身会解析到基类类型或成员，因此需要通过圆括号跳转到构造函数。

```case
tests/navigation/implicit_construction/05_member_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**委托构造函数**

委托构造函数中委托调用的圆括号可跳转到目标构造函数

在委托调用的左圆括号上，可跳转到委托构造函数的目标构造函数。构造函数名称本身会解析到类类型，因此需要通过圆括号跳转到目标构造函数。

```case
tests/navigation/implicit_construction/06_delegating_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承构造函数**

继承构造函数的声明可跳转到其引入的每个基类构造函数

在继承构造函数的声明（`using Base::Base;`）上执行“跳转到定义”，会列出它所引入的基类的每一个构造函数。

```case
tests/navigation/implicit_construction/07_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**返回值的隐式构造**

花括号形式的返回值可跳转到选中的构造函数

花括号形式的 `return {args}` 会隐式构造函数返回类型的对象；在花括号上执行“跳转到定义”，可跳转到选中的构造函数。

```case
tests/navigation/implicit_construction/08_return_values
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Lambda 初始化捕获**

Lambda 初始化捕获尚无法跳转到其移动构造函数

在 Lambda 初始化捕获的 `=` 上执行“跳转到定义”，无法跳转到构造捕获值的构造函数。

```case
tests/navigation/implicit_construction/09_lambda_captures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**结构化绑定（structured bindings）**

结构化绑定名称可跳转到绑定本身，而非底层字段或访问器

在结构化绑定名称上执行“跳转到定义”，会解析到绑定本身，而非该名称所对应的底层字段或访问器。

```case
tests/navigation/implicit_construction/10_structured_bindings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块导航

`import` 和 `module` 声明中的模块名称可跳转到声明该模块的接口单元。

<!-- BEGIN GENERATED ITEMS: navigation/modules -->

<!-- BEGIN CAPABILITY: supported clangd#2310 -->

**模块导入导航**

在 `import` 声明中的名称上执行“跳转到定义”，会打开导出该模块的接口单元；在已导入符号的使用处执行此操作，可跳转到该符号在该单元中的定义

```case
tests/navigation/modules/01_import_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**模块实现单元**

实现单元中的模块名称可跳转到声明该模块的接口单元

从导出函数的使用处可跳转到该函数在实现单元中的定义。目前尚不支持反向导航，即从接口中的模块名称跳转到实现该模块的单元。

```case
tests/navigation/modules/02_implementation_units
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**以点分隔的模块名称**

以点分隔的模块名称中，每一段都可导航到模块接口

在以点分隔的模块名称的任意一段上执行“跳转到定义”，都会跳转到该模块的接口单元；整个名称视为同一个引用。

```case
tests/navigation/modules/03_dotted_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 项目内导航

符号导航可跳转到项目中的其他文件：头文件中的声明、源文件中的定义或模块分区。

<!-- BEGIN GENERATED ITEMS: navigation/projects -->

<!-- BEGIN CAPABILITY: supported -->

**头文件中的声明**

从调用处可定位到函数在源文件中的定义和在头文件中的声明

```case
tests/navigation/projects/01_header_to_source
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

从模块函数的调用处可跳转到导出该函数的分区

```case
tests/navigation/projects/02_module_partition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**跨源文件的使用位置**

查找引用会汇集项目所有源文件中的使用位置，而不限于当前文件

函数在一个源文件中定义、在另一个同级源文件中调用时，查找引用会列出两处调用位置，以及共享头文件中的声明。

```case
tests/navigation/projects/03_uses_across_sources
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用层次结构

调用层次结构列出函数的调用方以及该函数调用的函数。

<!-- BEGIN GENERATED ITEMS: navigation/call_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**函数和方法**

自由函数和成员方法都支持打开调用层次结构

```case
tests/navigation/call_hierarchy/01_functions_and_methods
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传入调用**

传入调用列出函数的所有调用方；同一调用方多次调用该函数时，会列出每一处调用位置

```case
tests/navigation/call_hierarchy/02_incoming_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传出调用**

传出调用列出函数体内调用的所有函数，每个被调用函数对应一个条目

```case
tests/navigation/call_hierarchy/03_outgoing_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**调用层次结构条目详情**

调用层次结构条目仅包含名称，详情字段中没有函数签名，因此无法在层次结构中区分不同的重载

```case
tests/navigation/call_hierarchy/04_signature_details
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**成员函数的限定名称**

成员函数有对应的调用层次结构条目，但其名称字段仅包含方法名（`draw`），而非可用于将其与自由函数区分开的限定名称 `Circle::draw`

```case
tests/navigation/call_hierarchy/05_qualified_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**跟踪虚函数分派**

基类虚方法的传入调用不包含对派生类重写方法的调用；对重写方法的调用只归于该重写方法，不会归于它所重写的基类方法

```case
tests/navigation/call_hierarchy/06_virtual_dispatch
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1308 -->

**非函数目标**

对变量和枚举常量准备调用层次结构时，不返回任何结果

对变量或枚举常量准备调用层次结构时，不返回任何结果；该请求仅适用于自由函数和成员函数。

```case
tests/navigation/call_hierarchy/07_non_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 内的调用**

Lambda 体内的调用会出现在被调用函数的传入调用中，并归于包含该 Lambda 的函数

```case
tests/navigation/call_hierarchy/08_lambda_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2242 -->

**通过转发函数调用构造函数**

构造函数的传入调用不包含通过完美转发工厂调用该构造函数的位置

```case
tests/navigation/call_hierarchy/09_forwarding_factories
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型层次结构

类型层次结构列出类的基类以及从该类派生的类。

<!-- BEGIN GENERATED ITEMS: navigation/type_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**类型类别**

任何用户定义的类型都支持打开类型层次结构，包括类、结构体、枚举和联合体

```case
tests/navigation/type_hierarchy/01_type_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**超类型**

超类型列出类的所有直接基类，多重继承时也会列出每个基类

通过别名写出的基类，会列出该别名所指的类。

```case
tests/navigation/type_hierarchy/02_supertypes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**子类型**

子类型列出继承自某个基类的所有类，包括各个同级派生类

```case
tests/navigation/type_hierarchy/03_subtypes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板继承**

基类的子类型包括通过类模板（如 CRTP 包装类）继承自该基类的类

```case
tests/navigation/type_hierarchy/04_template_bases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#31 -->

**层次结构中的模板实参**

会列出类模板特化所产生的子类型，但条目名称仅包含模板名（`Derived`），不含可用于区分 `Derived<Foo>` 的模板实参

```case
tests/navigation/type_hierarchy/05_template_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档高亮

高亮显示当前文件中对光标所在符号的所有引用（`textDocument/documentHighlight`）。高亮与查找引用来自同一份索引，因此覆盖的名称与查找引用相同，在只读模式下同样可用；每处高亮都会标明该处代码是写入、读取还是声明该符号。

### 声明与使用

文件中该符号名称的所有出现位置都会高亮显示，宏也不例外。

<!-- BEGIN GENERATED ITEMS: document_highlight/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**声明与使用**

文件中光标所在符号的名称会在所有出现位置高亮显示，包括声明和定义处

```case
tests/document_highlight/symbols/01_declarations_and_uses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏名称与宏参数**

宏会在定义、展开、检测它的条件编译指令和 `#undef` 处高亮显示

宏参数中写出的名称会在其书写位置高亮显示；来自宏替换文本的名称则会高亮显示整个宏调用。在另一个宏的替换文本中使用的宏，既不会在该处高亮显示，也不会在另一个宏的调用处高亮显示。

```case
tests/document_highlight/symbols/03_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**控制流关键字**

尚不会高亮显示 `break` 或 `continue` 所离开的循环

```case
tests/document_highlight/symbols/08_control_flow
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 读取与写入

每处高亮都会标明该处代码是在读取还是写入符号。

<!-- BEGIN GENERATED ITEMS: document_highlight/access_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**读取与写入**

变量的每次使用都会高亮显示，赋值标记为写入，其他使用标记为读取

```case
tests/document_highlight/access_kinds/01_reads_and_writes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过非 const 引用传递的实参**

绑定到非 const 引用形参的实参被视为写入，而绑定到 const 引用、复制或取地址则被视为读取

```case
tests/document_highlight/access_kinds/03_reference_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员访问**

通过 `object.member` 写入时，会写入成员并读取对象

指定初始化器（designated initializer）或构造函数的成员初始化器会引用字段名称，但不算作写入。匿名联合体的字段与其他字段一样高亮显示。

```case
tests/document_highlight/access_kinds/06_member_access
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 模块单元

高亮显示适用于模块单元以及模块导出的名称。

<!-- BEGIN GENERATED ITEMS: document_highlight/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块单元**

模块单元和从模块导入的名称均支持高亮显示，也包括模块名称

```case
tests/document_highlight/modules/01_module_units
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 工作区符号

按名称在整个项目中搜索符号（`workspace/symbol`）。

### 查询

查询根据名称中的单词进行匹配，并通过查询语法缩小搜索范围。

<!-- BEGIN GENERATED ITEMS: workspace_symbol/queries -->

<!-- BEGIN CAPABILITY: supported -->

**按名称搜索**

查询会找到名称匹配的符号，并为成员显示其所属容器

```case
tests/workspace_symbol/queries/01_name_search
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#914 -->

**模糊匹配**

查询以子序列的形式匹配名称，并对齐到名称中的各个单词

`LinLis` 能找到 `LinkedList`，`pconf` 能找到 `parse_config`：查询中除首字母外的每个字母，要么紧接上一个匹配位置，要么位于名称中某个单词的开头，因此 `pcfg` 找不到任何结果，因为其中的 `f` 位于 `config` 中间。

```case
tests/workspace_symbol/queries/03_fuzzy_matching
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**查询语法**

引号、通配符、作用域和过滤条件都能收窄一次搜索

`"process"` 只匹配完整的名称，`proc*` 则匹配它通配到的一切；`io::*` 列出一个命名空间的成员，`io::**` 列出它的整棵子树；`kind:function` 只保留一种种类。各词项之间以空格分隔，可以组合使用。

```case
tests/workspace_symbol/queries/04_query_syntax
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**按修饰后的名称（链接器名称）搜索**

尚无法通过修饰后的链接器名称找到源代码中对应的函数

```case
tests/workspace_symbol/queries/05_mangled_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 限定名称

限定查询指定符号必须位于哪些作用域中。

<!-- BEGIN GENERATED ITEMS: workspace_symbol/scopes -->

<!-- BEGIN CAPABILITY: supported clangd#550 -->

**限定名称搜索**

带限定的查询指明符号必须位于哪些容器之内

`net::Socket` 能找到 `deep::net::Socket`：这些限定符必须按同样的顺序出现在符号的容器链中，其间和两端允许有别的容器；而以 `::` 开头则要求容器链与之完全一致。对限定查询的回复会写出限定名称，这样那些按查询文本过滤结果的编辑器就不会把它们滤掉。

```case
tests/workspace_symbol/scopes/01_qualified_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#931 -->

**有作用域枚举的枚举项查找**

枚举和其他容器一样，为自己的枚举项提供限定

```case
tests/workspace_symbol/scopes/02_enumerators
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 排序

名称完全匹配的结果排在首位。

<!-- BEGIN GENERATED ITEMS: workspace_symbol/ranking -->

<!-- BEGIN CAPABILITY: supported clangd#2253 -->

**别名排序**

名称完全一致的结果排在仅以该名称开头的结果之前

`Connection` 会先列出别名，随后才是 `ConnectionImpl`。

```case
tests/workspace_symbol/ranking/01_exact_name
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 结果

每个结果都会显示符号及其所属容器的名称。

<!-- BEGIN GENERATED ITEMS: workspace_symbol/results -->

<!-- BEGIN CAPABILITY: unsupported clangd#1344 -->

**重载消歧**

工作区符号结果省略了参数类型，导致无法区分重载

查询重载名称会找到所有重载，但每个条目都只显示名称本身，因此只有分别打开两个 `process` 结果对应的位置，才能区分它们。

```case
tests/workspace_symbol/results/01_overloads
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 未打开的文件

搜索范围涵盖项目中的所有文件，无论它们是否已在编辑器中打开。

<!-- BEGIN GENERATED ITEMS: workspace_symbol/projects -->

<!-- BEGIN CAPABILITY: supported -->

**搜索覆盖整个项目**

工作区符号搜索会返回尚未打开的项目文件中的匹配项

查询会找到 `other.cpp` 中的一个函数。此时 `main.cpp` 已打开，`other.cpp` 仍未打开，结果由后台索引提供。

```case
tests/workspace_symbol/projects/01_closed_files
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 切换源文件／头文件

打开与当前文件配对的文件：头文件的源文件、源文件的头文件、模块接口的实现单元、实现单元的接口。在 VS Code 中，可以从编辑器的右键菜单选择 **Switch Source/Header**，也可以按 `Alt+O`，或在命令面板中运行 **Clice: Switch Source/Header**；Neovim 中是 `:LspCliceSwitchSourceHeader`，命令行上则由 `clice query --method counterparts --path <file>` 给出同样的答案。Zed 不允许扩展添加命令，它自带的 _switch source header_ 只适用于 clangd。

候选文件来自构建和索引已经掌握的信息，因此给出答案无需编译任何东西：另一侧的同名文件（位于该文件自己的目录中，或者位于工作区的其他位置、但两者之间有包含关系）、一方声明而另一方定义的那些声明，以及模块声明。每个候选都附有理由。最佳候选是唯一的候选，或者胜过其余每个候选时，编辑器会直接打开它。胜过对方需要同时满足：在共享声明和同名这两项理由中，对方具备的它都具备；共享声明不比对方少；并且理由比对方多一项，或者在双方都有共享声明时达到对方的两倍，或者在双方都没有共享声明时具备对方没有的模块配对。否则编辑器会列出这些候选，供用户挑选。

### 头文件与源文件

头文件与定义其声明的源文件配对，源文件也与该头文件配对。

<!-- BEGIN GENERATED ITEMS: switch_source_header/counterparts -->

<!-- BEGIN CAPABILITY: supported -->

**头文件与源文件**

头文件与定义其声明的源文件配对，源文件也与该头文件配对

```case
tests/switch_source_header/counterparts/01_header_and_source
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**模板定义分开存放**

头文件与它为存放模板定义而包含的 `.tpp` 文件之间可以相互切换

```case
tests/switch_source_header/counterparts/02_template_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有配对的文件**

自身定义了全部所声明内容的头文件，以及 `.def` 片段，都没有配对文件

```case
tests/switch_source_header/counterparts/03_no_counterpart
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 目录布局

头文件可以跨目录找到对应的源文件，例如在 `include/` 和 `src/` 之间查找。

<!-- BEGIN GENERATED ITEMS: switch_source_header/layouts -->

<!-- BEGIN CAPABILITY: supported -->

**头文件与源文件分开存放**

`include/` 下的头文件能找到 `src/` 下的源文件，并把它排在另一个同名、但未定义该头文件任何声明的文件之前

在头文件所在目录之外，同名文件只有在两者中一方包含另一方时才算数（`tools/shape.cpp` 什么都没有包含，因此不是配对文件），而且其中只取最近的那个：`src/legacy/shape.cpp` 比 `src/shape.cpp` 深一层目录。

```case
tests/switch_source_header/layouts/01_split_directories
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 多个候选文件

如果有多个文件可以配对，配对关系最明确的文件会直接打开，其余候选仍会列在列表中。

<!-- BEGIN GENERATED ITEMS: switch_source_header/ranking -->

<!-- BEGIN CAPABILITY: supported -->

**定义分散在多个源文件中**

多个源文件分别定义同一头文件的声明时，直接选中定义数量明显最多的那个，其余的仍留在列表中

```case
tests/switch_source_header/ranking/01_scattered_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有明确的配对文件**

没有哪个候选明显胜过其他候选时，编辑器会列出所有候选及其理由，供用户挑选

```case
tests/switch_source_header/ranking/02_split_evenly
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 模块接口

模块接口单元与实现该模块的各个单元配对。

<!-- BEGIN GENERATED ITEMS: switch_source_header/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块接口与实现**

模块接口单元与实现该模块的各个单元配对，每个实现单元也与该接口配对

```case
tests/switch_source_header/modules/01_module_units
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**分区与其实现单元**

内部分区与定义其声明的同名实现单元之间可以直接相互切换

该实现单元还会列出其模块的主接口，排在分区之后。

```case
tests/switch_source_header/modules/02_partitions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
