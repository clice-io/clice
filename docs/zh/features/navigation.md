# 代码导航

## 跳转到定义

“跳转到定义”从名称跳往它所指实体的定义，也能从定义跳回声明。

<!-- BEGIN GENERATED ITEMS: navigation/definition -->

<!-- BEGIN CAPABILITY: supported -->

**仅有声明的符号**

找不到定义的符号会跳转到其声明

```case
tests/navigation/definition/01_declaration_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区导入**

模块分区导出的名称会跳转到该分区，导入语句中的分区名同样跳转到该分区

```case
tests/navigation/definition/02_partition_import
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义交替跳转**

从使用处跳转到定义，在声明与定义之间则跳往另一侧

声明与定义分离的函数，在声明处或定义处发起请求都会跳往另一处。没有独立声明的函数，在任何位置都会跳转到其定义。

```case
tests/navigation/definition/03_declaration_and_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**在 `#include` 指令上跳转到定义**

在包含指令上执行“跳转到定义”会打开所引用的文件

文件开头的包含指令与后文中的普通包含指令行为一致。

```case
tests/navigation/definition/05_include_directives
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**局部符号导航**

在局部变量或形参上执行“跳转到定义”会跳转到函数内的声明处

```case
tests/navigation/definition/06_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**类型未解析的变量**

若变量的类型无法解析，则无法从使用处跳转到该变量

即便变量的声明就在上文，在后续使用处执行“跳转到定义”也不会返回任何结果。

```case
tests/navigation/definition/07_error_recovery
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**纯虚函数**

在纯虚函数声明上执行“跳转到定义”会列出其所有重写函数

纯虚函数本身没有定义：光标停在其声明上时，“跳转到定义”会跳往提供定义的各个重写函数。通过纯虚函数发起的调用则会跳转到该声明，与任何没有定义的函数一致。

```case
tests/navigation/definition/10_pure_virtual
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到声明

从符号的使用处或定义跳转到其声明。C++ 中许多实体的声明与定义是分离的。

clice 会返回所有声明位置加上定义位置（内联定义的符号没有独立声明），再排除光标当前所在的位置，因此声明与定义之间也能像“跳转到定义”那样来回切换。如果一个函数有定义却没有对应的声明，则会回退到同一作用域内同名且尚无定义的函数——也就是因修改形参而与该定义脱节的原声明。

<!-- BEGIN GENERATED ITEMS: navigation/declaration -->

<!-- BEGIN CAPABILITY: supported -->

**函数**

使用处与独立定义都会跳转到函数原型

无论是从调用处还是从独立定义处，“跳转到声明”都会到达函数原型——函数原型正是与这两处非光标位置来回切换的。

```case
tests/navigation/declaration/01_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带前置声明的类类型**

既有前置声明又有后续定义的类，在使用处会同时给出这两处位置——前置声明会保留在声明集合中，不会因存在定义而舍弃

```case
tests/navigation/declaration/02_forward_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**静态数据成员**

静态成员的使用处会跳转到类内声明

静态数据成员在类内声明、在类外定义；在使用处执行“跳转到声明”会同时给出类内声明和类外定义。

```case
tests/navigation/declaration/03_static_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`extern` 变量**

外部变量的使用处会跳转到其声明

在 `extern` 变量的使用处会同时给出 `extern` 声明与定义性声明，因此从使用处总能到达头文件中的声明。

```case
tests/navigation/declaration/04_extern_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**多处声明**

使用处会跳转到每一处声明位置

函数在多处声明时，在使用处执行“跳转到声明”会列出全部声明，而不只是最近的一处。

```case
tests/navigation/declaration/05_redeclarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名写法上的表面差异**

形参名以及形参的顶层 `const` 不属于函数类型的一部分：下方示例中的声明与定义写法不同，但指代同一个函数，“跳转到声明”依然能将使用处连向函数原型

```case
tests/navigation/declaration/06_signature_spelling
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明已改动的定义**

当定义因改动而不再匹配原有声明时，在定义上执行“跳转到声明”仍能找到与之脱节的声明

只在一侧修改形参时，定义与头文件中的声明会变成两个不同的函数，该定义也就没有了对应的声明。此时在定义上执行“跳转到声明”，会退而查找同一作用域内同名、链接方式相同且尚无定义的函数——即同样需要同步修改的那条声明（若是 `static` 函数则限于当前文件内）。已有自身定义的重载不算候选，链接方式不同或位于另一个匿名命名空间中的函数也不算。在类内定义的方法从来没有单独的声明；模板和系统头文件中的函数则不做处理：它们就声明在定义处。

```case
tests/navigation/declaration/07_changed_signatures
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到类型定义

跳转到符号所属类型的定义，适用于变量、形参、字段以及任何带有类型的具名实体。类型别名会同时跳往别名本身及其所指的类型；类似指针的包装类型会同时跳往包装类型和被指向的类型；类型名本身则跳转到该类型自身。

<!-- BEGIN GENERATED ITEMS: navigation/type_definition -->

<!-- BEGIN CAPABILITY: supported -->

**变量与形参**

在局部变量或形参上执行“跳转到类型定义”会跳往其类型的定义

```case
tests/navigation/type_definition/01_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类与结构体字段**

在字段访问处执行“跳转到类型定义”会跳往该字段类型的定义

```case
tests/navigation/type_definition/02_fields
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由 `auto` 推导的变量**

在声明为 `auto` 的变量上执行“跳转到类型定义”会跳往推导出的类型，在 `auto` 关键字本身上执行也是如此

```case
tests/navigation/type_definition/03_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1026 -->

**智能指针所指类型导航**

在智能指针变量上执行“跳转到类型定义”会同时跳往包装类型和所指向的类型

```case
tests/navigation/type_definition/04_smart_pointers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型别名**

在使用类型别名的变量上执行“跳转到类型定义”，会同时跳往 `using` 或 `typedef` 声明及其所指的类型

别名模板则会跳往其目标类型所特化的模板。

```case
tests/navigation/type_definition/05_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定变量**

在结构化绑定（structured bindings）变量上执行“跳转到类型定义”会跳往所绑定成员的类型定义

```case
tests/navigation/type_definition/06_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指针、引用与数组**

“跳转到类型定义”会穿透指针、引用与数组，跳往底层元素类型的定义

```case
tests/navigation/type_definition/07_pointers_and_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this`**

在 `this` 上执行“跳转到类型定义”不返回任何结果，其他导航请求同样如此

写出 `this` 的代码本就位于它所指向的类中，跳转到该类并没有新信息；clangd 则会跳转到该类。

```case
tests/navigation/type_definition/11_this
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 跳转到实现

在虚方法上执行“跳转到实现”会列出下方各层派生类中的所有重写方法；在类上执行则会列出直接或间接继承自它的所有派生类。纯虚函数本身没有定义，在其声明上执行“跳转到定义”同样会列出所有重写方法。

<!-- BEGIN GENERATED ITEMS: navigation/implementation -->

<!-- BEGIN CAPABILITY: supported -->

**重写链**

“跳转到实现”会列出下方每一层的重写方法

在三层继承的重写链中，从基类执行“跳转到实现”会同时到达中间层和最底层的重写方法，从中间层执行则会到达最底层。

```case
tests/navigation/implementation/01_override_chain
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同级派生类的重写方法**

“跳转到实现”会列出各个同级派生类的重写方法

在虚方法上执行“跳转到实现”，会列出所有同级派生类中的重写方法。

```case
tests/navigation/implementation/02_sibling_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#854 -->

**未被重写的函数**

没有任何重写版本的函数以其自身定义作为实现

无论是否为虚函数，只要函数未被重写，在其声明上执行“跳转到实现”就会跳往其外部定义；在定义本身上则没有更下层的目标可跳。

```case
tests/navigation/implementation/03_non_virtual
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基类**

基类会跳转到所有派生类

在基类名上执行“跳转到实现”，会列出直接或通过其他派生类间接继承自它的所有类。

```case
tests/navigation/implementation/04_derived_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板鸭子类型导航**

在依赖调用（dependent call）上执行“跳转到实现”会跳往该调用在模板各个实例化中解析到的方法

这同样适用于函数模板、泛型 Lambda，以及按名称显式调用的转换运算符。

```case
tests/navigation/implementation/05_dependent_calls
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 查找引用

“查找引用”会列出所有提及该符号的位置，包括其各处声明。方法的引用包含通过它重写的基类方法发起、且可能动态分派到它的调用；类的引用包含其构造函数的名称。默认情况下编辑器最多收到 2000 条结果，并优先保留发起请求的文件内的引用（上限可在 [`[references]` 小节](../guide/configuration.md#references)中配置）；`clice query references` 则会列出全部结果。

<!-- BEGIN GENERATED ITEMS: navigation/references -->

<!-- BEGIN CAPABILITY: supported -->

**引用结果包含声明**

引用查询会将声明、独立定义与所有使用处全部返回，从任意一处都能找全该符号出现的全部位置

```case
tests/navigation/references/01_declarations_included
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1081 -->

**范围 for 循环中的引用**

在 `begin` 上执行“查找引用”会包含隐式调用它的范围 for 循环

范围 for 循环虽然没有写出 `begin()` 和 `end()` 的名字，但会调用它们；引用结果中以循环里的 `:` 代表这两次调用。

```case
tests/navigation/references/02_range_for
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**隐式构造与析构调用**

在构造函数或析构函数上执行“查找引用”只列出写出其名称的位置，不包含隐式运行它们的对象

`Blob b;` 会运行构造函数，离开作用域时会运行析构函数，但两处都没有写出函数名：若是把每个隐式构造或析构的对象都列出来，任何有局部变量的类的引用结果都会淹没在噪音中。

```case
tests/navigation/references/03_implicit_construction
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 -->

**通过转发函数的引用**

在构造函数上执行“查找引用”会包含完美转发工厂函数内部的构造位置，但不包含调用该工厂函数的位置

```case
tests/navigation/references/04_forwarding_factories
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#258 clangd#675 -->

**模板中的依赖引用**

在类成员上执行“查找引用”，结果包含以该类实例化的模板中对该成员的依赖调用位置

```case
tests/navigation/references/05_dependent_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2139 -->

**引用的读写分类**

查找引用的返回结果只包含位置，无法区分读取与写入；目前不支持为每条结果标注访问类别

```case
tests/navigation/references/06_access_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**引用所在的外层上下文**

返回给编辑器的每条引用只有位置信息，不附带其所在的外层函数

`clice query references` 会标出每条引用所在的函数。

```case
tests/navigation/references/07_enclosing_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签与 goto 引用**

在标签上执行“查找引用”会同时列出标签本身与所有跳往它的 `goto` 语句

```case
tests/navigation/references/08_labels
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1558 -->

**通过基类方法的虚调用**

在重写方法上执行“查找引用”，结果包含通过它重写的方法发起、可能分派到它的调用

在 `Shape&` 上调用 `shape.area()` 可能实际执行 `Circle::area`，因此会算在重写方法的引用中。通过 `Circle` 发起的调用绝不会执行 `Shape::area`，因此不算在基类被重写方法的引用中；重写方法体内的 `Shape::area()` 显式指名了被重写的基类方法，模板中的 `circle.Shape::scale(factor)` 也在实例化选定具体重载前就限定到了被重写的某个重载，这两处同样不算在重写方法的引用中。

```case
tests/navigation/references/13_override_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Objective-C 消息发送**

方法的引用包含所有可能发往它的消息：发给所属类的对象、发给其所重写方法所属父类的对象、通过其实现的协议发送，或发给 `id`

发给 `id` 或 `Class` 的消息可能到达具有相同选择器（selector）的任意方法，因此会归入每个同名方法的引用；发给 `super` 的消息则只到达父类的方法。分多段写出选择器的消息只列出一次，位置落在第一段上。

```case
tests/navigation/references/17_objective_c
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

在 `auto` 和 `decltype` 上执行导航会跳往它们所代表的类型。

<!-- BEGIN GENERATED ITEMS: navigation/deduced_types -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 推导类型导航**

在 `auto` 上执行“跳转到定义”会跳往推导出的类型，如同该处直接写出了类型名

```case
tests/navigation/deduced_types/01_auto_keyword
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2055 -->

**带指针与引用的 `auto`**

导航会剥去推导类型外层的指针、引用与数组：`auto*`、`const auto&`、`auto&&`、`decltype(auto)` 以及推导结果为指针的 `auto` 都会跳往底层的类

结构化绑定中的 `auto` 会跳往所分解对象的类型。

```case
tests/navigation/deduced_types/02_auto_declarators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` 类型导航**

在 `decltype` 上执行“跳转到定义”会跳往操作数所指的类型，并穿透该类型自身声明时用到的任意层 `decltype`

```case
tests/navigation/deduced_types/03_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导出的模板与别名**

推导为模板特化的 `auto` 会跳往主模板，或所选中的显式特化与偏特化；通过类型别名推导出的 `auto` 则跳往该别名

内置类型与 Lambda 闭包类型没有可跳往的声明，`new` 表达式中的 `auto` 也不会跳往任何位置。展开为 `auto` 的宏则会跳转到该宏本身。

```case
tests/navigation/deduced_types/04_deduced_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 形参与返回类型**

简写函数模板（abbreviated function template）形参中的 `auto` 会跳往各个实例化中的实参类型，返回类型中的 `auto` 则跳往实际返回的类型

尾置返回类型开头的 `auto` 会跳往箭头后面的类型。

```case
tests/navigation/deduced_types/05_parameters_and_returns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板中的 `auto`**

在模板内部，初始化器依赖模板参数的 `auto` 会跳往该初始化器解析所得的类型

若 `auto` 推导出的类型本身就是函数模板或类模板的模板参数，则会跳往该模板参数以及各个实例化中传入的模板实参。

```case
tests/navigation/deduced_types/06_dependent_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**库内部类型别名**

通过库内部别名推导出的 `auto` 会跳往该别名所指的类型；通过公开别名推导出的 `auto` 则跳往公开别名本身

像 `std::__detail::__unique_ptr_t` 这样仅供库自身保留的名称不是用户代码会写出的名字，因此 `auto` 会穿透它直达 `unique_ptr`。而像 `iterator` 这样的公开别名则是库面向用户提供的正式名称。

```case
tests/navigation/deduced_types/08_library_aliases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板

在模板内部，依赖模板参数的名称依然能跳转到可触达的各处声明。

<!-- BEGIN GENERATED ITEMS: navigation/templates -->

<!-- BEGIN CAPABILITY: supported -->

**依赖成员导航**

在从未实例化的模板中，访问依赖类型对象的成员会跳转到对应类模板上声明的成员

```case
tests/navigation/templates/01_dependent_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#212 -->

**模板特化导航**

在显式特化的名称上执行“跳转到定义”会跳往它所特化的主模板

```case
tests/navigation/templates/02_explicit_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖调用的候选重载**

可能命中多个重载的依赖调用会列出全部候选重载

每个候选各自给出跳转目标：有定义时跳往定义，没有定义时跳往声明。

```case
tests/navigation/templates/03_overload_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖表达式的成员**

对依赖下标、依赖调用或依赖 `auto` 变量的求值结果访问成员时，会跳转到相应类模板中声明的成员

若成员调用存在 `const` 重载，会根据对象本身的常量性选出对应的重载；若各重载的形参不同，则会选中与实参类型精确匹配的那一个；显式指定模板实参时则只匹配函数模板。

```case
tests/navigation/templates/04_dependent_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由已知实参排除的重载**

依赖调用会剔除类型已知的实参绝无可能匹配的候选重载

当标准转换、转换构造函数与转换函数都无法将该实参转换为某个重载的形参时，该重载即告排除；形参本身依赖模板参数的重载则会保留。

```case
tests/navigation/templates/11_known_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

写在宏实参中或由宏生成的名称与普通名称一样支持导航，宏的引用结果则涵盖每一处展开或测试该宏的位置。

<!-- BEGIN GENERATED ITEMS: navigation/macros -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参中的名称**

宏实参中写出的名称与宏外写出的名称一样支持导航

在宏实参中写出该名称的位置，定义与声明可以来回切换，后文中的使用处也会跳往该宏声明的函数。宏自身的名称无论在宏定义处还是宏调用处，都会跳转到该宏。

```case
tests/navigation/macros/01_macro_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏生成的名称**

由宏生成的名称会跳转到生成它的那次宏调用

通过 Token 拼接（token paste）拼出的名称或直接写在宏体中的名称，在使用处都会跳转到生成它的宏调用处，而宏调用处的宏名本身则跳转到宏定义。宏体内部的 Token在展开前没有确定的语义，在其上执行导航不返回任何结果。

```case
tests/navigation/macros/02_generated_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏引用**

搜索宏引用会涵盖宏展开、条件编译检测以及取消定义的位置

同名宏的每次 `#define` 各自算作独立符号，因此在 `#undef` 之后重新定义的宏只会收集属于它自己的使用处。

```case
tests/navigation/macros/04_macro_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#346 -->

**嵌套宏中的引用**

在宏上执行“查找引用”，结果包含其他实际展开过的宏体中提及它的位置

从未展开过的宏体不指代任何具体符号：其中名称的含义要到每次展开时才能确定。

```case
tests/navigation/macros/05_nested_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按翻译单元定义的头文件宏**

共享头文件中使用的宏，执行“跳转到定义”会到达包含它的各个翻译单元所展开的每一处宏定义

两个翻译单元都包含 `config.h`，其中一个定义了 `FAST`：头文件中的 `MODE` 在两个单元里分别展开为不同的宏定义，在该头文件未打开时发起“跳转到定义”会同时列出这两处定义。

```case
tests/navigation/macros/10_header_variants
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**已禁用的断言**

已禁用的 `assert` 内部的名称不会跳往任何位置

定义了 `NDEBUG` 时，`assert(...)` 会展开为空，其实参根本不参与编译：其中写出的名称没有可跳往的定义，也不会出现在引用结果中。clice 不会猜测它们原本指代什么。

```case
tests/navigation/macros/12_disabled_assert
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 隐式目标

跳转到隐式调用代码的定义。C++ 中许多语法结构会在背后隐式调用构造函数、运算符、类型转换函数等。从触发调用的语法位置（花括号、关键字、运算符 Token）直接跳往实际执行的函数，有助于看清代码真正在运行什么。

隐式导航需要一个无歧义的源码 Token——如果某个 Token 本身已有明确的跳转目标（例如变量名总是跳转到变量声明），就不能再挪作隐式调用的导航入口。

<!-- BEGIN GENERATED ITEMS: navigation/implicit_targets -->

<!-- BEGIN CAPABILITY: supported -->

**`override` / `final`**

`override` 和 `final` 会跳转到所重写的基类方法

在 `override` 或 `final` 说明符上执行“跳转到定义”，会跳往它重写的基类虚方法。

```case
tests/navigation/implicit_targets/01_override_and_final
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**`break` / `continue`**

`break` 和 `continue` 不会跳转到外层控制语句

在 `break` 或 `continue` 上执行“跳转到定义”，不会跳往其所属循环或 switch 语句的开头。

```case
tests/navigation/implicit_targets/02_break_and_continue
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`delete` 表达式**

`delete` 会跳转到它运行的析构函数与内存释放函数

在 `delete` 上执行“跳转到定义”，会同时跳往它调用的析构函数与类自定义的 `operator delete`。

```case
tests/navigation/implicit_targets/03_delete_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`new` 表达式**

`new` 会跳转到它运行的内存分配函数与构造函数

在 `new` 上执行“跳转到定义”，会同时跳往类重载的 `operator new` 以及同一表达式调用的构造函数。

```case
tests/navigation/implicit_targets/04_new_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载运算符**

重载运算符 Token 会跳转到其定义

在重载运算符 Token 上执行“跳转到定义”会跳往该运算符函数的定义。二元运算符、下标运算符、函数调用运算符与箭头运算符（`+`、`[]`、`()`、`->`）均支持解析。

```case
tests/navigation/implicit_targets/05_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C++20 重写比较运算符**

经重写合成的比较表达式会跳转到实际实现它的运算符

若比较表达式由 C++20 重写规则合成，在源码写出的运算符上执行“跳转到定义”会跳往实际提供实现的运算符：`!=` 跳往 `operator==`，`>` 跳往 `operator<=>`。

```case
tests/navigation/implicit_targets/06_rewritten_comparisons
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**用户定义字面量**

字面量后缀会跳转到对应的字面量运算符

在用户定义字面量的后缀上执行“跳转到定义”，会跳往它调用的 `operator""`。

```case
tests/navigation/implicit_targets/07_user_defined_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1931 -->

**隐式转换运算符**

条件表达式或 `!` 会跳转到其触发的转换运算符

在条件表达式的括号（`do` 循环中则是 `while` 关键字）上，或在操作数发生转换的 `!`、`&&`、`||`、`?` 上执行“跳转到定义”，会跳往用于检测操作数的用户定义转换运算符。

```case
tests/navigation/implicit_targets/08_implicit_conversions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型转换导航**

显式类型转换会跳转到它调用的构造函数或转换运算符

在 `static_cast`、C 风格强制转换的括号或函数式类型转换的类型名上执行“跳转到定义”，会跳往所选中的构造函数或调用的用户定义转换运算符。

```case
tests/navigation/implicit_targets/09_casts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**范围 for 循环**

范围 for 循环的冒号会跳转到 `begin()` 和 `end()`

在范围 for 循环的 `:` 上执行“跳转到定义”，会跳往为该范围选定的 `begin()` 和 `end()`。

```case
tests/navigation/implicit_targets/10_range_for
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`co_await` / `co_yield` / `co_return`**

协程关键字会跳转到它们隐式调用的方法

在 `co_await` 上执行“跳转到定义”会跳往 awaiter 的 `await_ready`、`await_suspend` 和 `await_resume`；在 `co_yield` 上跳往 promise 的 `yield_value`；在 `co_return` 上跳往 promise 的 `return_value`。

```case
tests/navigation/implicit_targets/11_coroutine_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 隐式构造

从初始化、返回语句、捕获与分解语法发起导航，可以跳往隐式选中的构造函数、聚合体定义或绑定。

<!-- BEGIN GENERATED ITEMS: navigation/implicit_construction -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数调用**

圆括号与花括号会跳转到选中的构造函数

在构造函数调用的左圆括号或左花括号上执行“跳转到定义”，会跳往重载决议选中的构造函数，同时支持 `T(args)` 和 `T{args}` 两种写法。

```case
tests/navigation/implicit_construction/01_constructor_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**拷贝/移动构造与赋值**

初始化或赋值中的 `=` 会跳转到它运行的函数

在赋值表达式的 `=` 上执行“跳转到定义”会跳往赋值运算符；在拷贝初始化或移动初始化（`T b = a;`）的 `=` 上执行则会跳往构造该变量的拷贝或移动构造函数。

```case
tests/navigation/implicit_construction/02_copy_and_move
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**CTAD**

使用 CTAD 的构造会跳转到所推导特化的构造函数

当类模板实参推导（class template argument deduction）选定某个特化时，在构造调用上执行“跳转到定义”会跳往选中的具体构造函数，而不只是类模板本身。

```case
tests/navigation/implicit_construction/03_deduced_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**聚合初始化**

聚合初始化的花括号会跳转到聚合体的定义

聚合体没有构造函数，因此在其初始化器的花括号上执行“跳转到定义”会跳往该聚合体的定义。

```case
tests/navigation/implicit_construction/04_aggregates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员初始化列表**

成员初始化器的圆括号会跳转到选中的基类或成员构造函数

初始化列表所调用的基类构造函数与成员构造函数，可从各个初始化器的左圆括号跳往。初始化器中的名称本身会解析到基类类型或成员变量，因此要跳往构造函数需在圆括号上触发。

```case
tests/navigation/implicit_construction/05_member_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**委托构造函数**

委托构造函数的圆括号会跳转到目标构造函数

委托构造函数的目标构造函数可从委托调用的左圆括号跳往。构造函数名本身会解析到所属类类型，因此要跳往目标构造函数需在圆括号上触发。

```case
tests/navigation/implicit_construction/06_delegating_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承构造函数**

继承构造函数声明会跳转到引入的每一个基类构造函数

在继承构造函数声明（`using Base::Base;`）上执行“跳转到定义”，会列出它从基类引入的每个构造函数。

```case
tests/navigation/implicit_construction/07_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**返回值隐式构造**

花括号形式的返回值会跳转到选中的构造函数

花括号形式的 `return {args}` 会隐式构造函数的返回类型；在花括号上执行“跳转到定义”会跳往选中的构造函数。

```case
tests/navigation/implicit_construction/08_return_values
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 初始化捕获**

Lambda 初始化捕获（init-capture）会跳转到构造捕获值的构造函数

在 Lambda 初始化捕获的 `=` 上执行“跳转到定义”，会跳往构造该捕获值的构造函数。

```case
tests/navigation/implicit_construction/09_lambda_captures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定**

结构化绑定的名称就是它自身的定义

在结构化绑定所声明的名称上执行“跳转到定义”会停留在绑定本身，与任何声明的名称一样；它绑定的成员或访问器并不是它的定义。

```case
tests/navigation/implicit_construction/10_structured_bindings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块导航

`import` 与 `module` 声明中的模块名会跳转到声明该模块的接口单元。

<!-- BEGIN GENERATED ITEMS: navigation/modules -->

<!-- BEGIN CAPABILITY: supported clangd#2310 -->

**模块导入导航**

在 `import` 声明中的模块名上执行“跳转到定义”会打开导出它的模块接口单元，使用导入的符号时也会跳往该单元中的定义

```case
tests/navigation/modules/01_import_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块实现单元**

模块接口单元与实现单元中的模块名可以相互跳转

实现单元中的模块名会跳往声明该模块的接口单元，接口单元中的模块名则会跳往实现该模块的各个单元。使用导出的函数时会跳往实现单元中的定义。

```case
tests/navigation/modules/02_implementation_units
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**点号分隔的模块名**

带点号模块名的每一段都会跳转到其接口单元

在点号分隔的模块名任意一段上执行“跳转到定义”，都会跳往该模块的接口单元；整个模块名算作同一处引用。

```case
tests/navigation/modules/03_dotted_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 项目内导航

导航会跟随符号进入项目的其他文件：头文件中的声明、源文件中的定义或模块分区。

<!-- BEGIN GENERATED ITEMS: navigation/projects -->

<!-- BEGIN CAPABILITY: supported -->

**头文件声明**

函数调用会关联源文件中的函数定义与头文件中的函数声明

```case
tests/navigation/projects/01_header_to_source
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

调用模块中的函数会落到导出该函数的模块分区

```case
tests/navigation/projects/02_module_partition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**跨源文件的使用**

“查找引用”会汇集项目中各个源文件内的使用处，而不局限于当前文件

在一个源文件中定义、并从同级源文件中调用的函数，会同时列出各处调用位置以及共享头文件中的声明。

```case
tests/navigation/projects/03_uses_across_sources
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板与宏内部的使用**

只要有源文件实例化模板或展开宏，无论该文件是否打开，“查找引用”与调用层次结构都会包含模板依赖代码和宏体在那里解析到的目标

`measure` 中的 `item.open()` 只有在以 `Box` 实例化 `measure` 后才指代 `Box::open`，`RESET_ALL` 宏体中的 `reset()` 也只在宏展开处才指代该函数：此处两者都发生在未打开的源文件中，打开头文件查询时依然能返回结果。

```case
tests/navigation/projects/09_instantiated_templates
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用层次结构

调用层次结构列出调用某个函数的调用方以及该函数调用的其他函数；通过该函数重写的方法发起的调用也会计入其调用方。每个条目都会显示函数的形参以区分重载。函数的局部变量与形参不生成条目：唯一能使用它们的就是该函数自身。

<!-- BEGIN GENERATED ITEMS: navigation/call_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**函数与方法**

自由函数与成员方法都支持打开调用层次结构

```case
tests/navigation/call_hierarchy/01_functions_and_methods
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传入调用**

传入调用列出函数的所有调用方；多次调用该函数的调用方会贡献每一处调用位置

```case
tests/navigation/call_hierarchy/02_incoming_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**传出调用**

传出调用列出函数体内调用的所有函数，每个被调函数对应一个条目

```case
tests/navigation/call_hierarchy/03_outgoing_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**调用层次结构条目详情**

调用层次结构条目在函数名旁显示形参列表，用以区分不同重载

```case
tests/navigation/call_hierarchy/04_signature_details
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员函数的限定名称**

成员函数的调用层次结构条目会在名称旁显示限定名称（`Circle::draw`），以便与自由函数区分

```case
tests/navigation/call_hierarchy/05_qualified_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过虚方法的调用**

通过虚方法发起的调用会到达所有可能分派到的重写方法，带作用域限定的调用则只到达它所指名的方法

在 `Base&` 上调用 `b.draw()` 可能执行 `Base::draw` 或 `Derived::draw`，因此两者的传入调用都会列出它。在 `Derived&` 上调用 `d.draw()` 只会执行 `Derived::draw` 或更下层的重写方法，绝不会执行 `Base::draw`，因此只有重写方法会列出它。`Base::draw()` 无论写在重写方法体内还是别处都只执行 `Base::draw`（而 `Group` 中的 `child->draw()` 则可能到达任一方法）；`Logged` 的重写方法中由宏展开出的 `Base::draw()` 同样只执行 `Base::draw`。

```case
tests/navigation/call_hierarchy/06_virtual_dispatch
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1308 -->

**非函数目标**

变量、字段与枚举常量也支持打开调用层次结构

在变量、字段或枚举常量上发起调用层次结构请求时，会生成一个条目，其传入调用即为使用该符号的各个函数。

```case
tests/navigation/call_hierarchy/07_non_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 内部的调用**

写在 Lambda 体内的调用会出现在被调函数的传入调用中，并归属到包含该 Lambda 的外层函数

```case
tests/navigation/call_hierarchy/08_lambda_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 -->

**通过转发函数的构造函数调用**

构造函数的传入调用包含运行它的完美转发工厂函数，但不包含调用该工厂函数的位置

```case
tests/navigation/call_hierarchy/09_forwarding_factories
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数局部符号**

函数的局部变量与形参不生成调用层次结构条目

能使用它们的只有声明它们的函数本身，即使生成条目也只有一个事先已知的调用方。而在函数内声明的类，其字段仍会生成条目，因为类自身的方法会使用这些字段。

```case
tests/navigation/call_hierarchy/15_function_locals
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型层次结构

类型层次结构列出类的基类以及继承自它的派生类。

<!-- BEGIN GENERATED ITEMS: navigation/type_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**类型类别**

任何用户定义类型均可打开类型层次结构，包括类、结构体、枚举与联合体

```case
tests/navigation/type_hierarchy/01_type_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**超类型**

超类型列出类的所有直接基类，多重继承下也会逐一列出每个基类

以类型别名写出的基类会列出该别名所指的类。

```case
tests/navigation/type_hierarchy/02_supertypes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**子类型**

子类型列出继承自该基类的所有派生类，涵盖各个同级派生类

```case
tests/navigation/type_hierarchy/03_subtypes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板继承**

基类的子类型包含通过类模板（如 CRTP 包装类）继承自它的类

```case
tests/navigation/type_hierarchy/04_template_bases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由模板参数指定的基类**

继承自自身模板参数的类模板，会成为用来实例化它的每个类的子类型

```case
tests/navigation/type_hierarchy/09_dependent_bases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档高亮

高亮显示当前文件中光标下符号的所有引用（`textDocument/documentHighlight`）。文档高亮与“查找引用”共用同一份索引，因此覆盖相同的名称，在只读模式下同样可用；变量、字段或形参在代码中发生写入或读取的位置会分别标为写入或读取，其余高亮（包括只能靠模板实例化或宏展开解析出的使用处）则标为普通高亮。

### 声明与使用

文件中出现该符号的每一处名称都会高亮，宏也不例外。

<!-- BEGIN GENERATED ITEMS: document_highlights/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**声明与使用**

文件中光标下符号的每一处名称都会高亮，涵盖各处声明与定义

```case
tests/document_highlights/symbols/01_declarations_and_uses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏名与宏实参**

宏会在宏定义、各处展开、检测它的条件编译指令以及 `#undef` 处高亮

写在宏实参中的名称就在写出它的位置高亮；写在宏替换体中的名称则会高亮整处宏调用。出现在另一个宏替换体中的宏会在该替换体中按普通文本出现位置高亮，但不会在外层宏的调用处高亮。

```case
tests/document_highlights/symbols/03_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**控制流关键字**

`break` 或 `continue` 不会高亮它所跳出的循环

```case
tests/document_highlights/symbols/08_control_flow
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 读取与写入

每处高亮都会标明该处代码是在读取还是写入该符号。

<!-- BEGIN GENERATED ITEMS: document_highlights/access_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**读取与写入**

变量的每处使用都会高亮，赋值标为写入，其余使用标为读取

```case
tests/document_highlights/access_kinds/01_reads_and_writes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按可变引用传递的实参**

绑定到非 const 引用形参的实参标为写入，而绑定到 const 引用、按值拷贝或取地址则标为读取

```case
tests/document_highlights/access_kinds/03_reference_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员访问**

通过 `object.member` 写入时，成员标为写入，对象标为读取

指定初始化器（designated initializer）或构造函数的成员初始化器会将对应字段标为写入。匿名联合体的字段与普通字段一样支持高亮。

```case
tests/document_highlights/access_kinds/06_member_access
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 模块单元

在模块单元中以及模块导出的名称上同样支持文档高亮。

<!-- BEGIN GENERATED ITEMS: document_highlights/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块单元**

在模块单元中以及从模块导入的名称（包括模块名本身）上均支持高亮

```case
tests/document_highlights/modules/01_module_units
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 工作区符号

按名称在编辑器打开的所有项目中搜索符号（`workspace/symbol`），命名空间作用域下具有内部链接（internal linkage）的名称（如 `static` 符号与匿名命名空间成员）也涵盖在内。每条结果带有与大纲一致的符号类别，函数结果还会附带形参列表。

### 查询

查询按名称中的单词匹配，并可借助查询语法收窄搜索范围。

<!-- BEGIN GENERATED ITEMS: workspace_symbols/queries -->

<!-- BEGIN CAPABILITY: supported -->

**按名称搜索**

查询会找出名称匹配的符号，成员符号还会附带所属容器

```case
tests/workspace_symbols/queries/01_name_search
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#914 -->

**模糊匹配**

查询以对齐单词边界的子序列方式匹配名称

`LinLis` 可以匹配 `LinkedList`，`pconf` 可以匹配 `parse_config`：除首字母外，查询中的每个字母要么紧接上一段连续字符，要么落在名称中某个单词的开头；因此 `pcfg` 什么也匹配不到——其中的 `f` 落在了 `config` 中间。

```case
tests/workspace_symbols/queries/03_fuzzy_matching
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**查询语法**

引号、通配符、作用域与筛选条件可以收窄搜索范围

`"process"` 只精确匹配完整名称，`proc*` 按通配符匹配；`io::*` 列出命名空间的直接成员，`io::**` 列出整个子树；`kind:function` 只保留指定类别。多个条件以空格分隔组合使用。VS Code 的符号搜索只会把查询中的第一项发给服务器，再用该项在本地过滤返回结果，因此在 VS Code 中第二项及之后的条件以及单独使用的筛选器都查不到结果。

```case
tests/workspace_symbols/queries/04_query_syntax
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**按修饰名（链接器名称）搜索**

修饰后的链接器名称不会解析回源码中的函数

```case
tests/workspace_symbols/queries/05_mangled_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 限定名称

带限定的查询可指明符号必须位于哪些外层作用域中。

<!-- BEGIN GENERATED ITEMS: workspace_symbols/scopes -->

<!-- BEGIN CAPABILITY: supported clangd#550 -->

**限定名称搜索**

带限定的查询可指定符号所属的外层容器

`net::Socket` 能找到 `deep::net::Socket`：各限定名只需按顺序出现在符号的容器链中，前后和中间允许夹有其他容器；加上前导 `::` 则要求容器链完全吻合。对带限定的查询返回的结果会写出限定名称，因此按查询文本再次过滤结果的编辑器不会把它们滤掉。

```case
tests/workspace_symbols/scopes/01_qualified_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#931 -->

**带作用域枚举项查找**

枚举类型与其他容器一样可以限定其枚举项

```case
tests/workspace_symbols/scopes/02_enumerators
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 排序

完全匹配的名称排在最前。

<!-- BEGIN GENERATED ITEMS: workspace_symbols/ranking -->

<!-- BEGIN CAPABILITY: supported clangd#2253 -->

**别名排序**

拼写完全一致的名称排在仅以它为前缀的名称之前

搜索 `Connection` 时，同名类型别名排在前面，`ConnectionImpl` 排在其后。

```case
tests/workspace_symbols/ranking/01_exact_name
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 结果

每条结果都会标出符号名称及其所在的外层容器。

<!-- BEGIN GENERATED ITEMS: workspace_symbols/results -->

<!-- BEGIN CAPABILITY: supported clangd#1344 -->

**区分重载函数**

工作区符号结果会显示每个函数的形参，便于区分重载

查询重载函数名会返回每一个重载，并各自附带形参列表：两条 `process` 结果一眼即可区分。

```case
tests/workspace_symbols/results/01_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**内部链接名称**

搜索会找出各个文件中声明于命名空间作用域的内部链接名称，但不包含函数内的局部符号

`static` 变量或匿名命名空间的成员虽然仅对所在文件可见，但仍属于项目声明的符号：每个声明它的文件都会在搜索结果中列出一条。局部变量则不会列出。

```case
tests/workspace_symbols/results/06_internal_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 未打开的文件

搜索范围覆盖项目中的所有文件，无论是否已在编辑器中打开。

<!-- BEGIN GENERATED ITEMS: workspace_symbols/projects -->

<!-- BEGIN CAPABILITY: supported -->

**覆盖整个项目的搜索**

工作区符号搜索会返回未打开项目文件中的匹配结果

只打开 `main.cpp` 而未打开 `other.cpp` 时，查询依然能找到 `other.cpp` 中的函数——结果由后台索引提供。

```case
tests/workspace_symbols/projects/01_closed_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同一代码库的多份检出副本**

并排打开同一代码的两份检出目录时，每个符号只列出一次，取正在编辑的那个文件夹中的副本

同一仓库的两个工作树（worktree）作为工作区文件夹同时打开时，相同文本的相同位置上会有完全相同的符号。搜索对每个符号只列出一次，取打开了文档的那个文件夹——也就是当前正在编辑的文件夹中的副本。若某个文件夹中的文件内容不同，它就持有独立的符号，也会列出。

```case
tests/workspace_symbols/projects/03_worktree_copies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 切换源文件与头文件

打开与当前文件配对的文件：头文件对应的源文件、源文件对应的头文件、模块接口对应的实现单元，或实现单元对应的模块接口。在 VS Code 中，可在编辑器右键菜单选择 **Switch Header and Source (Module Interface and Implementation)**、按快捷键 `Alt+O`，或在命令面板运行 **clice: Switch Header and Source (Module Interface and Implementation)**；在 Neovim 中可运行 `:LspCliceSwitchSourceHeader`；命令行下运行 `clice query counterparts <file>` 也会返回相同的结果。Zed 不支持由扩展注册命令，其内置的 _switch source header_ 仅支持 clangd。

候选文件直接取自构建和索引已知的信息，无需为此触发任何编译，来源包括：另一侧的同名文件（位于当前文件所在目录，或位于项目其他目录但两者存在包含关系）、一方声明而另一方定义的共享声明，以及模块声明关系。每个候选文件都附有入选理由。当只有一个候选文件，或最佳候选胜过其余每一个候选时，编辑器会直接打开它——所谓胜过另一候选，是指在“共享声明”与“同名”两项理由上对方具备的它都具备，共享声明数量不少于对方，并且满足以下至少一条：多出一项理由；双方都有共享声明且数量至少是对方的两倍；或双方都没有共享声明，但具备对方所没有的模块配对关系。否则，编辑器会列出所有候选文件供用户选择。

### 头文件与源文件

头文件与定义其声明的源文件配对，源文件也与该头文件配对。

<!-- BEGIN GENERATED ITEMS: counterparts/basics -->

<!-- BEGIN CAPABILITY: supported -->

**头文件与源文件**

头文件与定义其声明的源文件配对，源文件也与该头文件配对

```case
tests/counterparts/basics/01_header_and_source
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**分离存放的模板定义**

头文件与它为存放模板定义而包含的 `.tpp` 文件可相互切换

```case
tests/counterparts/basics/02_template_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无配对的文件**

自行定义了所声明全部内容的头文件以及 `.def` 片段没有配对文件

```case
tests/counterparts/basics/03_no_counterpart
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 目录布局

头文件能够跨目录（如 `include/` 与 `src/`）找到对应的源文件。

<!-- BEGIN GENERATED ITEMS: counterparts/layouts -->

<!-- BEGIN CAPABILITY: supported -->

**头文件与源文件分目录存放**

`include/` 下的头文件能找到 `src/` 下的源文件，并排在另一个未定义其任何声明的同名文件之前

在头文件自身所在目录之外，同名文件只有在双方存在包含关系时才计入候选（`tools/shape.cpp` 未包含任何文件，因此不算配对文件），且只取目录距离最近的那个：`src/legacy/shape.cpp` 比 `src/shape.cpp` 多深一层目录。

```case
tests/counterparts/layouts/01_split_directories
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 多个候选文件

存在多个可配对的文件时，配对关系最明确的一个会直接打开，其余候选仍保留在列表中。

<!-- BEGIN GENERATED ITEMS: counterparts/ranking -->

<!-- BEGIN CAPABILITY: supported -->

**定义分散在多个源文件中**

多个源文件分别定义了同一头文件的声明时，定义数量明显最多的那个会直接选中，其余文件保留在候选列表中

```case
tests/counterparts/ranking/01_scattered_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无明显占优的配对文件**

没有哪个候选明显胜出时，编辑器会列出所有候选及其入选理由供用户选择

```case
tests/counterparts/ranking/02_split_evenly
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 模块接口

模块接口单元与实现该模块的各个单元配对。

<!-- BEGIN GENERATED ITEMS: counterparts/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块接口与实现单元**

模块接口单元与实现该模块的各个单元配对，每个实现单元也与该接口单元配对

```case
tests/counterparts/modules/01_module_units
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**分区与对应的实现单元**

内部模块分区与定义其声明的同名实现单元可直接相互切换

该实现单元还会将所属模块的主接口单元列在分区之后。

```case
tests/counterparts/modules/02_partitions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
