# 代码导航

## 跳转到定义

<!-- BEGIN GENERATED ITEMS: go_to_definition -->

<!-- BEGIN CAPABILITY: supported -->

**跨 TU 跳转到定义**

从一个翻译单元中的使用位置，可以跳转到同一项目中另一个源文件提供的定义，查找范围覆盖整个项目，而不局限于当前文件

```snap
tests/snap/navigation/go_to_definition/01_def_cross_tu/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**定义与声明相互跳转**

在声明与定义之间相互跳转

从使用位置发起请求会跳转到定义，从声明或定义处发起请求则会跳转到另一处。对于没有单独声明的内联符号，跳转目标仍是其定义。

```snap
tests/snap/navigation/go_to_definition/02_def_decl_alternate.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**仅有声明的符号导航**

对于只有声明的符号，如纯虚函数、`extern` 变量、类内静态常量，会跳转到该声明，而不是不返回任何结果

```snap
tests/snap/navigation/go_to_definition/03_def_declaration_only.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**在 `#include` 指令上跳转到定义**

在包含指令上执行跳转到定义，会打开其引用的文件

文件开头的包含指令与文件后面普通的包含指令行为一致。

```snap
tests/snap/navigation/go_to_definition/04_def_include/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**局部符号导航**

在局部变量或参数上执行跳转到定义，会跳转到它在函数体内的声明

```snap
tests/snap/navigation/go_to_definition/05_def_local_symbol.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏包装导航**

宏实参中直接写出的名称以其书写位置为定位点，因此可以像普通位置一样在定义与声明之间相互跳转；从后续使用位置发起跳转时，也能穿过宏包装，定位到它所声明的函数

```snap
tests/snap/navigation/go_to_definition/06_def_macro_wrapper.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏生成的名称**

通过 Token 拼接生成的名称在源码中没有独立的书写位置，因此以生成它的宏调用为定位点：该调用就是它的定义位置，在普通代码中使用该名称时，可以跳转回该调用

```snap
tests/snap/navigation/go_to_definition/07_def_macro_generated.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏体导航**

宏体中写出的 Token 只有在宏展开时才获得具体含义，因此在其上执行导航不会返回结果，而宏调用处的 Token 始终会跳转到所展开的宏

```snap
tests/snap/navigation/go_to_definition/08_def_macro_body.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**错误恢复**

变量类型无法解析时，无法跳转到该变量的声明

当变量的类型名无法解析时，即使仍然记录了变量自身的声明，目前在该变量的后续使用位置执行跳转到定义也不会返回结果。

```snap
tests/snap/navigation/go_to_definition/09_def_error_recovery.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**待决成员导航**

在从未实例化的模板中，访问待决类型（dependent type）对象的成员时，可以跳转到对应类模板中声明的成员

```snap
tests/snap/navigation/go_to_definition/10_def_dependent_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#212 -->

**模板特化导航**

在显式特化的名称上执行跳转到定义，会跳转到该特化本身；不支持从该特化进一步跳转到它所特化的主模板

```snap
tests/snap/navigation/go_to_definition/11_def_template_spec.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 推导类型导航**

在 `auto` 上执行“跳转到定义”，会跳转到它推导出的类型，就像在该处直接写出了这个类型一样

在该关键字上执行“跳转到类型定义”会到达同一个类型，从它发起“查找引用”会列出该类型的各处使用。关键字本身不算作对该类型的使用：从类型发起“查找引用”时不会列出它。

```snap
tests/snap/navigation/go_to_definition/12_def_auto_keyword.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖调用的重载候选**

依赖调用可能命中多个重载时，会逐一列出这些重载

每个候选各自给出结果：有定义的跳转到定义，没有定义的跳转到声明。

```snap
tests/snap/navigation/go_to_definition/13_def_overload_candidates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖表达式的成员**

在依赖下标、调用或 `auto` 变量的结果上访问成员时，解析到类模板中声明的成员

调用存在 `const` 重载时，由对象是否为 const 决定解析到哪一个重载；参数不同的各个重载则全部列出。

```snap
tests/snap/navigation/go_to_definition/14_def_dependent_expression.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2055 -->

**指针和引用形式的 `auto`**

导航会剥去推导类型外层的指针、引用和数组：`auto*`、`const auto&`、`auto&&`、`decltype(auto)` 以及推导为指针的 `auto` 都会跳转到对应的类

结构化绑定（structured bindings）的 `auto` 跳转到被分解对象的类型。

```snap
tests/snap/navigation/go_to_definition/15_def_auto_declarators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` 类型导航**

在 `decltype` 上执行“跳转到定义”，会跳转到其操作数所对应的类型；如果该类型本身也是用 `decltype` 声明的，会沿着它继续追溯

```snap
tests/snap/navigation/go_to_definition/16_def_decltype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导出的模板和别名**

推导为模板特化的 `auto` 跳转到该模板，或跳转到它选中的显式特化或偏特化；经由别名推导出的 `auto` 跳转到该别名

内置类型和 Lambda 的闭包类型没有可跳转的声明，`new` 表达式中的 `auto` 也不会跳转到任何位置。展开为 `auto` 的宏则跳转到该宏本身。

```snap
tests/snap/navigation/go_to_definition/17_def_auto_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 参数和返回类型**

简写函数模板（abbreviated function template）参数中的 `auto` 跳转到其唯一一次实例化所用的类型；`auto` 返回类型则跳转到实际返回的类型

如果模板以多种类型实例化，参数中的 `auto` 不会跳转到其中任何一个。尾置返回类型（trailing return type）开头的 `auto` 跳转到箭头之后的类型。

```snap
tests/snap/navigation/go_to_definition/18_def_auto_params_returns.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未实例化模板中的 `auto`**

在模板内部，初始化器依赖模板参数的 `auto` 会跳转到该初始化器在类模板上解析出的类型

如果解析出的类型就是模板参数本身，无论模板如何实例化，都跳转到该模板参数。

```snap
tests/snap/navigation/go_to_definition/19_def_auto_dependent.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 隐式目标

跳转到隐式调用的代码定义。在 C++ 中，许多语法结构会隐式调用构造函数、运算符、转换函数等。从语法结构（花括号、关键字、运算符 Token）跳转到实际调用的函数，对于理解究竟执行了哪些代码至关重要。

隐式导航要求源码中的 Token 没有歧义。如果 Token 已有明确的“转到定义”目标（例如，变量名始终跳转到其声明），就不能再用它来导航到隐式调用。

<!-- BEGIN GENERATED ITEMS: implicit_targets -->

<!-- BEGIN CAPABILITY: unsupported -->

**`override` / `final`**

`override` 和 `final` 尚不能跳转到被重写的基类方法

在 `override` 或 `final` 说明符上执行“转到定义”，无法跳转到被重写的基类虚方法。

```snap
tests/snap/navigation/implicit_targets/01_override_final.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**`break` / `continue`**

`break` 和 `continue` 尚不能跳转到其所在的控制语句

在 `break` 或 `continue` 上执行“转到定义”，无法跳转到其控制的循环或 switch 语句的头部。

```snap
tests/snap/navigation/implicit_targets/02_break_continue.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`delete` 表达式**

`delete` 尚不能跳转到所调用的析构函数

在 `delete` 上执行“转到定义”，无法跳转到它调用的析构函数。

```snap
tests/snap/navigation/implicit_targets/03_delete_dtor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`new` 表达式**

`new` 可跳转到重载的内存分配函数，但不能跳转到构造函数

在 `new` 上执行“转到定义”，可跳转到类中重载的 `operator new`。返回结果不包含同一表达式调用的构造函数。

```snap
tests/snap/navigation/implicit_targets/04_new_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载运算符**

重载运算符的 Token 可跳转到其定义

在重载运算符的 Token 上执行“转到定义”，可跳转到该运算符的定义。二元、下标、调用和箭头运算符（`+`、`[]`、`()`、`->`）均可解析。

```snap
tests/snap/navigation/implicit_targets/05_operator_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C++20 重写运算符**

重写后的比较可跳转到实际实现该比较的运算符

对于按 C++20 重写规则合成的比较，在源码中写出的运算符上执行“转到定义”，可跳转到实际实现该比较的运算符：`!=` 跳转到 `operator==`，`>` 跳转到 `operator<=>`。

```snap
tests/snap/navigation/implicit_targets/06_rewritten_operator.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**用户定义字面量（user-defined literals）**

字面量后缀尚不能跳转到对应的用户定义字面量运算符

在用户定义字面量的后缀上执行“转到定义”，无法跳转到对应的 `operator""`。

```snap
tests/snap/navigation/implicit_targets/07_udl.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1931 -->

**隐式转换运算符**

转换上下文尚不能跳转到所调用的转换运算符

在执行用户定义转换的上下文（条件、`!`、显式的 `bool(...)`）中执行“转到定义”，无法跳转到对应的转换运算符。

```snap
tests/snap/navigation/implicit_targets/08_conversion_context.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**强制类型转换导航**

调用构造函数的强制类型转换可跳转到所选的构造函数

调用用户定义转换运算符的 `static_cast` 尚不能跳转到该运算符。

```snap
tests/snap/navigation/implicit_targets/09_cast_conversion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**范围 for 循环（range-based for）**

范围 for 循环中的冒号尚不能跳转到 `begin()` 或 `end()`

在范围 for 循环的 `:` 上执行“转到定义”，无法跳转到为该范围选用的 `begin()` 或 `end()`。

```snap
tests/snap/navigation/implicit_targets/10_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`co_await` / `co_yield` / `co_return`**

`co_yield` 可跳转到 promise 对象的方法，其他协程关键字则不能

在 `co_yield` 上执行“转到定义”，可跳转到 promise 对象的 `yield_value`。`co_await` 和 `co_return` 关键字尚不能跳转到等待器（awaiter）或 promise 对象的方法。

```snap
tests/snap/navigation/implicit_targets/11_coroutine.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 隐式构造

从初始化、返回、捕获和分解语法执行导航，可跳转到隐式选定的构造函数、聚合类型定义或绑定。

<!-- BEGIN GENERATED ITEMS: implicit_construction -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数调用**

圆括号和花括号可跳转到所选的构造函数

在构造函数调用的左圆括号或左花括号上执行“跳转到定义”，可跳转到重载决议选中的构造函数，适用于 `T(args)` 和 `T{args}` 两种形式。

```snap
tests/snap/navigation/implicit_construction/01_constructor_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**拷贝／移动构造与赋值**

赋值中的 `=` 可跳转到赋值运算符，拷贝初始化和移动初始化中的 `=` 则不可以

在赋值中的 `=` 上执行“跳转到定义”，可跳转到赋值运算符。引入拷贝初始化或移动初始化的 `=`（`T b = a;`）属于初始化语法，而非运算符调用，目前尚无法解析。

```snap
tests/snap/navigation/implicit_construction/02_copy_move.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**CTAD**

使用 CTAD 的构造调用可跳转到推导出的特化的构造函数

当类模板实参推导选中特化时，在构造函数调用上执行“跳转到定义”，可跳转到选中的构造函数，而不只是类模板。

```snap
tests/snap/navigation/implicit_construction/03_ctad.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**聚合初始化**

聚合初始化的花括号可跳转到聚合类型的定义

聚合类型没有构造函数，因此在其初始化器的花括号上执行“跳转到定义”，会跳转到聚合类型的定义。

```snap
tests/snap/navigation/implicit_construction/04_aggregate_init.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**成员初始化列表**

成员初始化器的圆括号可跳转到选中的基类或成员构造函数

在初始化列表中各初始化器的左圆括号上，可跳转到该初始化器调用的基类或成员构造函数。初始化器名称本身会解析到基类类型或成员，因此需要通过圆括号跳转到构造函数。

```snap
tests/snap/navigation/implicit_construction/05_member_init.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**委托构造函数**

委托构造函数中委托调用的圆括号可跳转到目标构造函数

在委托调用的左圆括号上，可跳转到委托构造函数的目标构造函数。构造函数名称本身会解析到类类型，因此需要通过圆括号跳转到目标构造函数。

```snap
tests/snap/navigation/implicit_construction/06_delegating_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承构造函数**

继承构造函数的声明可跳转到引入的每一个基类构造函数

在继承构造函数的声明（`using Base::Base;`）上执行“跳转到定义”，会列出它所引入的基类的每一个构造函数。

```snap
tests/snap/navigation/implicit_construction/07_inherited_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**返回值的隐式构造**

花括号形式的返回值可跳转到选中的构造函数

花括号形式的 `return {args}` 会隐式构造函数返回类型的对象；在花括号上执行“跳转到定义”，可跳转到选中的构造函数。

```snap
tests/snap/navigation/implicit_construction/08_return_construction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Lambda 初始化捕获**

Lambda 初始化捕获尚无法跳转到其移动构造函数

在 Lambda 初始化捕获的 `=` 上执行“跳转到定义”，无法跳转到构造捕获值的构造函数。

```snap
tests/snap/navigation/implicit_construction/09_lambda_capture.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**结构化绑定（structured bindings）**

结构化绑定名称可跳转到绑定本身，而非底层字段或访问器

在结构化绑定名称上执行“跳转到定义”，会解析到绑定本身，而非该名称所对应的底层字段或访问器。

```snap
tests/snap/navigation/implicit_construction/10_structured_binding.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到声明

从符号的使用处或定义处跳转到其声明。在 C++ 中，许多实体的声明和定义是分开的。

clice 返回声明和定义的位置，并排除光标当前所在的位置；内联定义的符号没有单独的声明。因此，声明与定义位置会像“跳转到定义”一样交替跳转。

<!-- BEGIN GENERATED ITEMS: go_to_declaration -->

<!-- BEGIN CAPABILITY: supported -->

**跨 TU 跳转到声明**

在使用处执行“跳转到声明”可找到其他文件中的位置：函数原型位于共享头文件中，定义则单独放在同级源文件中，从另一文件中的使用处跳转时，两者都会列出

```snap
tests/snap/navigation/go_to_declaration/01_decl_cross_tu/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数**

从使用处和单独给出的定义处跳转到函数原型

从调用处和单独给出的定义处执行“跳转到声明”，都能到达函数原型；原型会与这两处非当前光标位置交替跳转。

```snap
tests/snap/navigation/go_to_declaration/02_decl_function_prototype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**前置声明的记录类型**

如果一个类先有前置声明，随后才有定义，从使用处跳转时会同时列出两者；前置声明仍保留在声明集合中，不会因为有了定义而被移除

```snap
tests/snap/navigation/go_to_declaration/03_decl_forward_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**静态数据成员**

从静态成员的使用处跳转到类内声明

静态数据成员在类内声明、类外定义；在使用处执行“跳转到声明”时，会同时列出类内声明和定义。

```snap
tests/snap/navigation/go_to_declaration/04_decl_static_member.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`extern` 变量**

从外部变量的使用处跳转到其声明

从 `extern` 变量的使用处跳转时，会同时列出 `extern` 声明和定义性声明，因此始终可以从使用处到达头文件中的声明。

```snap
tests/snap/navigation/go_to_declaration/05_decl_extern_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**多处声明**

从使用处跳转到所有声明位置

当一个实体在多处声明时，在使用处执行“跳转到声明”会列出所有声明位置，而不只是最近的一处。

```snap
tests/snap/navigation/go_to_declaration/06_decl_multiple.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名的表面差异**

参数名和参数的顶层 `const` 都不属于函数类型的一部分：下方的声明与定义对同一个函数采用了不同的写法，但“跳转到声明”仍能从使用处跳转到函数原型

```snap
tests/snap/navigation/go_to_declaration/07_decl_signature_mismatch.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到实现

<!-- BEGIN GENERATED ITEMS: go_to_implementation -->

<!-- BEGIN CAPABILITY: supported -->

**重写链**

沿重写链逐级跳转到实现

在三级重写链中，从每个方法执行“跳转到实现”都会到达下一级的重写方法：从基类到中间类，再从中间类到叶子类。

```snap
tests/snap/navigation/go_to_implementation/01_impl_virtual_chain.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同级重写**

跳转到实现时列出所有同级重写方法

在虚方法上执行“跳转到实现”，会列出各个同级派生类中的所有重写方法。

```snap
tests/snap/navigation/go_to_implementation/02_impl_virtual_siblings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#854 -->

**非虚函数**

尚不能从非虚函数声明跳转到单独给出的定义

在非虚函数声明上执行“跳转到实现”，无法到达其单独给出的定义，会返回空结果。

```snap
tests/snap/navigation/go_to_implementation/03_impl_nonvirtual_def.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基类**

从基类跳转到所有派生类

在基类名称上执行“跳转到实现”，会列出从该类派生的类。

```snap
tests/snap/navigation/go_to_implementation/04_impl_base_derived.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**模板鸭子类型导航**

尚不能将依赖调用解析到已知实例化中的方法

这适用于函数模板和泛型 Lambda，但目前两者都不会返回实现目标。

```snap
tests/snap/navigation/go_to_implementation/05_impl_template_duck_type.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到类型定义

跳转到符号的类型定义。适用于变量、参数、字段以及其他具有类型的命名实体。当类型是类型别名或类似指针的包装类型时，应解开别名或包装，跳转到其底层类型或所指向类型的定义。

<!-- BEGIN GENERATED ITEMS: go_to_type_definition -->

<!-- BEGIN CAPABILITY: supported -->

**变量和参数**

对局部变量或参数执行“跳转到类型定义”，可跳转到其类型的定义

```snap
tests/snap/navigation/go_to_type_definition/01_typedef_variables.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类和结构体字段**

对字段访问执行“跳转到类型定义”，可跳转到该字段类型的定义

```snap
tests/snap/navigation/go_to_type_definition/02_typedef_field.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由 `auto` 推导类型的变量**

对以 `auto` 声明的变量执行“跳转到类型定义”，会跳转到其推导出的类型，与在 `auto` 关键字上执行时相同

```snap
tests/snap/navigation/go_to_type_definition/03_typedef_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1026 -->

**跳转到智能指针所指向的类型**

对智能指针变量执行“跳转到类型定义”，会跳转到包装类型本身；尚不支持解开包装并跳转到所指向的类型

```snap
tests/snap/navigation/go_to_type_definition/04_typedef_smart_pointer.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**类型别名**

对使用类型别名的变量执行“跳转到类型定义”，会跳转到 `using` 或 `typedef` 声明；目前还无法解开别名并跳转到底层类型的定义

```snap
tests/snap/navigation/go_to_type_definition/05_typedef_alias.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）变量**

对结构化绑定执行“跳转到类型定义”，可跳转到所绑定成员的类型定义

```snap
tests/snap/navigation/go_to_type_definition/06_typedef_structured_binding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指针、引用和数组**

“跳转到类型定义”会透过指针、引用和数组，跳转到元素类型的定义

```snap
tests/snap/navigation/go_to_type_definition/07_typedef_pointer_reference.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 查找引用

<!-- BEGIN GENERATED ITEMS: find_references -->

<!-- BEGIN CAPABILITY: supported -->

**跨 TU 查找引用**

查找引用也会收集其他文件中的使用位置：对于在一个源文件中定义、在另一个同级源文件中调用的函数，结果会同时列出两处调用位置以及共享头文件中的声明，而不局限于当前文件中的使用位置

```snap
tests/snap/navigation/find_references/01_refs_cross_tu/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**引用结果中的声明**

引用查询会返回声明、在声明之外编写的定义以及所有使用位置，因此从符号的任一位置都能跳转到它的所有相关位置

```snap
tests/snap/navigation/find_references/02_refs_include_declaration.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1081 -->

**范围 for 循环引用**

对 `begin` 查找引用只会返回它自身的声明；隐式调用它的范围 for 循环不会出现在引用结果中

```snap
tests/snap/navigation/find_references/03_refs_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**隐式构造函数和析构函数调用**

对构造函数查找引用只会返回显式引用位置；隐式调用该构造函数或对应析构函数的对象定义不会出现在结果中

```snap
tests/snap/navigation/find_references/04_refs_implicit_construction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 clangd#1872 -->

**通过转发函数产生的引用**

对构造函数查找引用时，结果不包含通过完美转发工厂间接调用它的位置

```snap
tests/snap/navigation/find_references/05_refs_forwarding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#258 clangd#675 -->

**模板中的依赖引用**

对成员查找引用时，结果不包含模板中依赖于模板参数的调用位置，即使用该成员所属的类实例化了模板也不例外

```snap
tests/snap/navigation/find_references/06_refs_dependent_context.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2139 -->

**引用的读写分类**

引用查询的响应只包含位置，无法据此区分写入和读取；尚不支持为每条结果标注访问类型

```snap
tests/snap/navigation/find_references/07_refs_read_write.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#177 -->

**引用的上下文**

每条引用只返回位置，不附带所在函数的名称，因此结果中除文件和行号外没有其他上下文信息

```snap
tests/snap/navigation/find_references/08_refs_enclosing_context.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏引用**

宏引用搜索会包含宏展开、条件测试和取消定义的位置

同一名称的每次 `#define` 都被视为独立的符号，因此在 `#undef` 之后重新定义的宏只会收集自身的使用位置。

```snap
tests/snap/navigation/find_references/09_refs_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#346 -->

**嵌套宏引用**

对宏查找引用时，结果不包含其他宏定义体中对该宏的引用

```snap
tests/snap/navigation/find_references/10_refs_macro_in_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签和 goto 引用**

查找标签的引用时，会列出标签本身以及所有跳转到该标签的 `goto`

```snap
tests/snap/navigation/find_references/11_refs_label_goto.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用层次结构

<!-- BEGIN GENERATED ITEMS: call_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**调用层次结构准备**

自由函数和成员函数都支持准备调用层次结构，并以光标处的实体为定位点创建条目

```snap
tests/snap/navigation/call_hierarchy/01_calls_prepare.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传入调用**

传入调用列出函数的所有调用者；同一调用者多次调用该函数时，会列出每个调用位置

```snap
tests/snap/navigation/call_hierarchy/02_calls_incoming.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传出调用**

传出调用列出函数体内调用的所有函数，每个被调用函数对应一个条目

```snap
tests/snap/navigation/call_hierarchy/03_calls_outgoing.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**调用层次结构条目详情**

调用层次结构条目仅包含名称，详情字段中没有附带函数签名，因此无法在层次结构中区分重载

```snap
tests/snap/navigation/call_hierarchy/04_calls_detail_signature.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**成员函数的限定名称**

成员函数会生成调用层次结构条目，但名称字段仅包含函数名（`draw`），不包含可将其与自由函数区分开的限定名称 `Circle::draw`

```snap
tests/snap/navigation/call_hierarchy/05_calls_qualified_name.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**跟踪虚函数分派**

基类虚函数的传入调用不包含通过派生类中的重写函数发起的调用；对重写函数的调用仅归属于该重写函数，不会归属于被它重写的基类函数

```snap
tests/snap/navigation/call_hierarchy/06_calls_virtual_dispatch.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1308 -->

**非函数目标**

对变量和枚举常量准备调用层次结构时，不返回任何结果

对变量或枚举常量准备调用层次结构时，不返回任何结果；该请求仅适用于自由函数和成员函数。

```snap
tests/snap/navigation/call_hierarchy/07_calls_non_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 内的调用**

Lambda 体内的调用会出现在被调用函数的传入调用中，并归属于包含该 Lambda 的函数

```snap
tests/snap/navigation/call_hierarchy/08_calls_lambda.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2242 -->

**通过转发函数调用构造函数**

构造函数的传入调用不包含通过完美转发工厂函数间接调用它的位置

```snap
tests/snap/navigation/call_hierarchy/09_calls_forwarding_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型层次结构

<!-- BEGIN GENERATED ITEMS: type_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**类型层次结构准备**

准备类型层次结构时，可以在任意用户定义的类型标记上定位条目，包括类、结构体、枚举和联合体

```snap
tests/snap/navigation/type_hierarchy/01_types_prepare.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**超类型**

超类型列出类的所有直接基类，包括多重继承中派生类型的每个基类

通过别名写出的基类，会列出该别名所指的类。

```snap
tests/snap/navigation/type_hierarchy/02_types_supertypes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**子类型**

子类型列出从某个基类派生的所有类，涵盖各个同级派生类型

```snap
tests/snap/navigation/type_hierarchy/03_types_subtypes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板继承**

基类的子类型包括通过类模板（例如 CRTP 包装类）从该基类派生的类

```snap
tests/snap/navigation/type_hierarchy/04_types_template_inheritance.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#31 -->

**层次结构中的模板实参**

类模板特化产生的子类型会被列出，但条目名称仅包含模板名（`Derived`），不包含用于区分 `Derived<Foo>` 的模板实参

```snap
tests/snap/navigation/type_hierarchy/05_types_template_args.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 工作区符号

按名称在整个项目中搜索符号（`workspace/symbol`）。

<!-- BEGIN GENERATED ITEMS: workspace_symbol -->

<!-- BEGIN CAPABILITY: supported -->

**基本的工作区全局符号搜索**

工作区符号搜索匹配名称时不区分大小写

查询以子序列的形式匹配符号名称，并对齐到名称中的各个单词，不区分大小写：函数、类型、枚举项和宏都在搜索范围内；没有匹配项时返回空列表，不会报错。

```snap
tests/snap/workspace_symbol/workspace_symbol/01_basic_search.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**搜索覆盖整个项目**

工作区符号搜索会返回项目中未打开文件里的匹配项

查询也会返回编辑器中尚未打开的项目文件里的符号：此处 `other.h` 一直未打开，因此其中的匹配结果由后台索引提供。

```snap
tests/snap/workspace_symbol/workspace_symbol/02_cross_file_search/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1344 -->

**重载消歧**

工作区符号结果省略了参数类型，导致无法区分重载

查询重载名称会找到所有重载，但每个条目都只显示名称本身，因此只有分别打开两个 `process` 结果对应的位置，才能区分它们。

```snap
tests/snap/workspace_symbol/workspace_symbol/03_overload_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#914 -->

**模糊匹配**

查询以子序列的形式匹配名称，并对齐到名称中的各个单词

`LinLis` 能找到 `LinkedList`，`pconf` 能找到 `parse_config`：除首字母外，查询中的每个字母要么接着前一个字母连成一段，要么落在名称中某个单词的词首，所以 `pcfg` 什么也找不到——它的 `f` 落在了 `config` 中间。名称完全一致的排在最前，其次是以查询开头的名称，最后才是匹配位置更深入名称内部的。

```snap
tests/snap/workspace_symbol/workspace_symbol/04_fuzzy_matching.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#550 -->

**限定名称搜索**

带限定的查询指明符号必须位于哪些容器之内

`net::Socket` 能找到 `deep::net::Socket`：这些限定符必须按同样的顺序出现在符号的容器链中，其间和两端允许有别的容器；而以 `::` 开头则要求容器链与之完全一致。对限定查询的回复会写出限定名称，这样那些按查询文本过滤结果的编辑器就不会把它们滤掉。

```snap
tests/snap/workspace_symbol/workspace_symbol/05_qualified_search.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#931 -->

**有作用域枚举的枚举项查找**

枚举和其他容器一样，为自己的枚举项提供限定

```snap
tests/snap/workspace_symbol/workspace_symbol/06_enum_scope.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2253 -->

**别名排序**

名称完全一致的结果排在仅以该名称开头的结果之前

`Connection` 会先列出别名，随后才是 `ConnectionImpl`。

```snap
tests/snap/workspace_symbol/workspace_symbol/07_alias_priority.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**按修饰后的名称（链接器名称）搜索**

尚无法通过修饰后的链接器名称找到源代码中对应的函数

```snap
tests/snap/workspace_symbol/workspace_symbol/08_mangled_name.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**查询语法**

引号、通配符、作用域和过滤条件都能收窄一次搜索

`"process"` 只匹配完整的名称，`proc*` 则匹配它通配到的一切；`io::*` 列出一个命名空间的成员，`io::**` 列出它的整棵子树；`kind:function` 只保留一种种类。各词项之间以空格分隔，可以组合使用。

```snap
tests/snap/workspace_symbol/workspace_symbol/09_query_syntax.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块导航

<!-- BEGIN GENERATED ITEMS: module_navigation -->

<!-- BEGIN CAPABILITY: supported clangd#2310 -->

**模块导入导航**

在 `import` 声明中的名称上执行“转到定义”，会打开导出该模块的模块接口单元；在导入符号的使用处执行此操作，会跳转到该符号在该单元中的定义

```snap
tests/snap/navigation/module_navigation/01_module_import_name/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区导航**

在分区导入语句中冒号后的分区名称上执行“转到定义”，会打开声明该分区的分区单元

```snap
tests/snap/navigation/module_navigation/02_module_partition_import/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**模块接口与实现导航**

在实现单元的模块名称（`module m;`）上执行“转到定义”，会跳转到声明该模块的接口单元；尚不支持从接口名称反向跳转到实现

```snap
tests/snap/navigation/module_navigation/03_module_iface_impl/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**以点分隔的模块名称**

以点分隔的模块名称中，每一段都可导航到模块接口

在以点分隔的模块名称的任意一段上执行“转到定义”，都会跳转到该模块的接口单元；整个名称视为同一个引用。

```snap
tests/snap/navigation/module_navigation/04_module_dotted/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档高亮

高亮显示当前文件中对光标所在符号的所有引用（`textDocument/documentHighlight`）。高亮与查找引用来自同一份索引，因此覆盖的名称与查找引用相同，在只读模式下同样可用；每处高亮都会标明该处代码是写入、读取还是声明该符号。

<!-- BEGIN GENERATED ITEMS: document_highlight -->

<!-- BEGIN CAPABILITY: supported -->

**文档引用高亮**

当前文件中光标所在符号的每一处名称都会高亮显示，包括它的声明和定义

```snap
tests/snap/document_highlight/document_highlight/01_highlight_references.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**读写访问**

高亮区分写入和读取：赋值、复合赋值、自增或自减都会写入该名称

其余用法都是读取。声明既不算读取也不算写入，无论是否带初始化器，都按普通文本高亮显示。

```snap
tests/snap/document_highlight/document_highlight/02_highlight_read_write.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过可变引用传递的实参**

绑定到非 const 左值引用形参的实参算作写入

const 引用形参或按值传递的形参读取实参，对实参取地址同样算作读取。转发引用（forwarding reference，`T&&`、`auto&&`）可以绑定任何实参，也算作读取，除非可变参数转发函数把实参继续传给可变引用形参。

```snap
tests/snap/document_highlight/document_highlight/03_highlight_reference_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过重载运算符写入**

类重载的赋值、复合赋值、自增和自减运算符与内置运算符一样，会写入左操作数

其他运算符读取左操作数；调用成员函数时，对象同样算作读取，显式对象形参（explicit object parameter）也不例外。如果运算符或成员函数以可变引用接收某个操作数或实参（流提取运算符就是如此），该操作数或实参算作写入。运算符本身在其声明处以及每个使用它的表达式中高亮显示。

```snap
tests/snap/document_highlight/document_highlight/04_highlight_overloaded_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**字段与成员访问**

通过 `object.member` 写入时，写入的是成员，对象本身只被读取

指定初始化器或构造函数的成员初始化器会指明它所初始化的字段，但不算作写入：初始化不是赋值。匿名联合体的字段与其他字段一样高亮显示。

```snap
tests/snap/document_highlight/document_highlight/05_highlight_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏名称与宏参数**

宏在其定义、展开处、检测它的条件编译指令以及它的 `#undef` 处高亮显示

写在宏参数中的名称在书写位置高亮；由宏的替换文本拼出的名称则高亮整个宏调用。在另一个宏的替换文本中使用的宏，既不会在那里高亮，也不会在那个宏的调用处高亮。

```snap
tests/snap/document_highlight/document_highlight/06_highlight_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类名、构造函数与析构函数**

类在其名称指代它的每一处高亮显示，析构函数的 `~Name` 中也是如此；构造函数或析构函数高亮显示自身的声明和使用处

构造表达式如果没有写出构造函数名称（`Session(7)` 写出的是类名），则通过它的圆括号关联到构造函数。

```snap
tests/snap/document_highlight/document_highlight/07_highlight_constructors.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板与特化**

模板在其自身的声明和每个使用处高亮显示；显式特化或偏特化是独立的符号

模板参数在其所属模板内高亮显示。在模板内部，经由依赖类型访问的成员会与它可能指代的那些成员一同高亮。

```snap
tests/snap/document_highlight/document_highlight/08_highlight_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 捕获与结构化绑定**

被 Lambda 捕获的变量在捕获列表和 Lambda 体中高亮显示；初始化捕获是独立的变量

结构化绑定引入的每个名称都是独立的符号。

```snap
tests/snap/document_highlight/document_highlight/09_highlight_lambdas_bindings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**控制流 Token 高亮**

控制流关键字尚无相关的文档高亮

```snap
tests/snap/document_highlight/document_highlight/10_highlight_control_flow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块单元中的高亮**

高亮可用于模块单元，也可用于从模块导入的名称，包括模块名称本身

```snap
tests/snap/document_highlight/document_highlight/11_highlight_module_unit/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 切换源文件／头文件

打开与当前文件配对的文件：头文件的源文件、源文件的头文件、模块接口的实现单元、实现单元的接口。在 VS Code 中，可以从编辑器的右键菜单选择 **Switch Source/Header**，也可以按 `Alt+O`，或在命令面板中运行 **Clice: Switch Source/Header**；Neovim 中是 `:LspCliceSwitchSourceHeader`，命令行上则由 `clice query --method counterparts --path <file>` 给出同样的答案。Zed 不允许扩展添加命令，它自带的 _switch source header_ 只适用于 clangd。

候选文件来自构建和索引已经掌握的信息，因此给出答案无需编译任何东西：另一侧的同名文件（位于该文件自己的目录中，或者位于工作区的其他位置、但两者之间有包含关系）、一方声明而另一方定义的那些声明，以及模块声明。每个候选都附有理由。最佳候选是唯一的候选，或者胜过其余每个候选时，编辑器会直接打开它。胜过对方需要同时满足：在共享声明和同名这两项理由中，对方具备的它都具备；共享声明不比对方少；并且理由比对方多一项，或者在双方都有共享声明时达到对方的两倍，或者在双方都没有共享声明时具备对方没有的模块配对。否则编辑器会列出这些候选，供用户挑选。

<!-- BEGIN GENERATED ITEMS: switch_source_header -->

<!-- BEGIN CAPABILITY: supported -->

**头文件与其源文件**

头文件与定义其所声明内容的同名源文件之间可以直接相互切换

```snap
tests/snap/switch_source_header/switch_source_header/01_header_source/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件与源文件分开存放**

`include/` 下的头文件能找到 `src/` 下的源文件，并把它排在另一个同名、但未定义该头文件任何声明的文件之前

在头文件所在目录之外，同名文件只有在两者中一方包含另一方时才算数（`tools/shape.cpp` 什么都没有包含，因此不是配对文件），而且其中只取最近的那个：`src/legacy/shape.cpp` 比 `src/shape.cpp` 深一层目录。

```snap
tests/snap/switch_source_header/switch_source_header/02_split_layout/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**定义分散在多个源文件中**

多个源文件分别定义同一头文件的声明时，直接选中定义数量明显最多的那个，其余的仍留在列表中

```snap
tests/snap/switch_source_header/switch_source_header/03_scattered_definitions/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有明确的配对文件**

没有哪个候选明显胜过其他候选时，编辑器会列出所有候选及其理由，供用户挑选

```snap
tests/snap/switch_source_header/switch_source_header/04_split_evenly/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块接口与实现**

模块接口单元与实现该模块的各个单元配对，每个实现单元也与该接口配对

```snap
tests/snap/switch_source_header/switch_source_header/05_module_units/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**分区与其实现单元**

内部分区与定义其声明的同名实现单元之间可以直接相互切换

该实现单元还会列出其模块的主接口，排在分区之后。

```snap
tests/snap/switch_source_header/switch_source_header/06_module_partition/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有配对的文件**

自身定义了全部所声明内容的头文件，以及 `.def` 片段，都没有配对文件

```snap
tests/snap/switch_source_header/switch_source_header/07_no_counterpart/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板定义分开存放**

头文件与它为存放模板定义而包含的 `.tpp` 文件之间可以相互切换

```snap
tests/snap/switch_source_header/switch_source_header/08_template_definitions/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
