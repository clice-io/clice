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

<!-- BEGIN CAPABILITY: unsupported clangd#2055 -->

**`auto` 推导类型导航**

目前还无法从 `auto` 关键字跳转到其推导出的类型

```snap
tests/snap/navigation/go_to_definition/12_def_auto_keyword.cpp
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

<!-- BEGIN CAPABILITY: partial -->

**继承构造函数**

继承构造函数的声明可跳转到引入的某一个基类构造函数

在继承构造函数的声明（`using Base::Base;`）上执行“跳转到定义”，可跳转到基类构造函数。当基类声明了多个构造函数时，返回结果会指向其中一个，而不会列出全部构造函数。

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

<!-- BEGIN CAPABILITY: unsupported -->

**由 `auto` 推导类型的变量**

由 auto 推导类型的变量目前还无法跳转到其推导出的类型定义

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

工作区符号搜索通过子串匹配名称，不区分大小写

查询会匹配名称中包含查询字符串的所有符号，不区分大小写：函数、类型、枚举项和宏都在搜索范围内；没有匹配项时返回空列表，不会报错。

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

<!-- BEGIN CAPABILITY: unsupported clangd#914 -->

**模糊匹配**

工作区符号搜索尚不支持基于单词边界的模糊匹配

匹配采用不区分大小写的子串检查：`LinLis` 无法找到 `LinkedList`，`pcfg` 也无法找到 `parse_config`。包括宏在内，所有类型的符号都不支持按单词边界处的首字母匹配。

```snap
tests/snap/workspace_symbol/workspace_symbol/04_fuzzy_matching.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#550 -->

**部分限定名称搜索**

符号仅按名称本身匹配：即使存在 `deep::net::Socket`，搜索 `net::Socket` 也找不到结果，其他带有限定符前缀的形式同样如此

```snap
tests/snap/workspace_symbol/workspace_symbol/05_qualified_search.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#931 -->

**有作用域枚举的枚举项查找**

使用限定名称查询枚举项尚无结果

```snap
tests/snap/workspace_symbol/workspace_symbol/06_enum_scope.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2253 -->

**别名排序**

尚未对匹配的别名及其所指向的声明进行排序

目前尚未对结果进行排序。

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

<!-- BEGIN CAPABILITY: partial -->

**以点分隔的模块名称**

以点分隔的模块名称中，只有首段支持导航到模块接口

在以点分隔的模块名称的首段上执行“转到定义”，会跳转到该模块的接口单元；点号后的各段尚无法单独解析。

```snap
tests/snap/navigation/module_navigation/04_module_dotted/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档高亮

高亮显示当前文件中对光标所在符号的所有引用（`textDocument/documentHighlight`）。

<!-- BEGIN GENERATED ITEMS: document_highlight -->

<!-- BEGIN CAPABILITY: unsupported -->

**文档引用高亮**

文档高亮尚未实现，因此声明和使用处都不会高亮显示

```snap
tests/snap/navigation/document_highlight/01_highlight_references.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**符号高亮的读写分类**

文档高亮尚不报告读写访问类型

```snap
tests/snap/navigation/document_highlight/02_highlight_read_write.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**控制流 Token 高亮**

控制流关键字尚无相关的文档高亮

```snap
tests/snap/navigation/document_highlight/03_highlight_control_flow.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 切换源文件／头文件

<!-- BEGIN GENERATED ITEMS: switch_source_header -->

<!-- BEGIN CAPABILITY: unsupported -->

**源文件与头文件切换**

源文件与头文件切换尚未实现，因此用户无法在配对文件之间直接跳转

```snap
tests/snap/navigation/switch_source_header/01_switch_source_header.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
