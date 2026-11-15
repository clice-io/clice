# 内联提示

<!-- The cards in the sections below are generated from the documented cases
     in tests/inlay_hints/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

clice 为代码中未显式写出的信息渲染内联标注：调用处的参数名、推导出的类型、按位置进行聚合初始化时对应的字段名，以及右花括号或 `#endif` 所结束的结构。每种提示在 `[inlay_hints]` 配置节中都有各自的开关；下文各节会说明哪些提示默认关闭。

## 参数名提示

调用处会显示每个实参绑定的参数名，除非实参本身已表明该名称。

<!-- BEGIN GENERATED ITEMS: inlay_hints/parameter_hints -->

<!-- BEGIN CAPABILITY: supported -->

**实参名称**

字面量实参会显示它绑定的参数名

```case
tests/inlay_hints/parameter_hints/01_argument_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**提示抑制**

具名实参和 `/*name=*/` 注释会抑制参数名提示

与参数同名的实参，或未加限定的同名成员，无需显示提示；带限定符的名称或通过对象访问的成员不算匹配。写出其他参数名的注释不会抑制任何提示。

```case
tests/inlay_hints/parameter_hints/03_suppression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1123 -->

**可修改引用标记**

`&` 标记按非 const 左值引用传递的实参

该标记有独立的开关 `inlay_hints.mutable_references`：关闭参数名提示后它仍会保留，显示在空名称前面。

```case
tests/inlay_hints/parameter_hints/05_reference_markers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2324 -->

**转发解析**

经包装函数转发的参数包会解析出目标的参数名

直接透传无需 `std::forward` 也能解析，位于固定的前置与后置实参之间的参数包会解析到它落入的参数，转交给可调用对象的参数包则会解析到其调用运算符。最终用于初始化标量的参数包没有可显示的参数名。

```case
tests/inlay_hints/parameter_hints/06_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**来自定义的参数名**

声明中未命名的参数采用更早声明给出的名称，没有则取定义中的名称；开头的下划线会去掉

声明和定义都为参数命名时，以声明为准。

```case
tests/inlay_hints/parameter_hints/07_definition_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1734 clangd#1742 -->

**函数指针与调用运算符**

间接调用仍会显示参数名

```case
tests/inlay_hints/parameter_hints/08_function_objects
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖调用（dependent calls）**

即使只能在模板内部确定被调用者，仍会显示参数名

候选函数按实参个数和类型已知的实参匹配；只有唯一留下的候选函数才会显示参数名，仍可能命中多个重载的调用则不显示提示，不做猜测。

```case
tests/inlay_hints/parameter_hints/10_dependent_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2620 -->

**调用处的宏**

写成宏的实参会显示提示；宏体内生成的调用不显示提示

对象式宏（object-like macro）算作源码中写出的一个实参。跨多个实参的宏没有可锚定提示的位置；作为宏实参编写的代码会保留提示，其中嵌套宏的参数也是如此，但嵌套宏自身的宏体不显示提示。

```case
tests/inlay_hints/parameter_hints/12_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2248 -->

**带前缀的参数名**

实参名与去掉单字母前缀后的参数名相同时，无需显示提示

将 `param` 传给 `aParam`、将 `buffer` 传给 `pBuffer` 都视为同名。`inlay_hints.prefixed_names` 会重新显示这些提示。

```case
tests/inlay_hints/parameter_hints/16_near_matches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1364 -->

**继承构造函数**

`using Base::Base` 引入的构造函数按基类声明的写法显示参数名

```case
tests/inlay_hints/parameter_hints/17_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单字母参数名**

只接受一个参数的函数不显示单字母参数名

像单字母 `x`（如 `push(int x)`）这样的名称并未提供调用本身之外的信息；有多个参数时，这些字母用于区分彼此，因此会保留。`inlay_hints.single_letter_names` 会将它们全部显示出来。

```case
tests/inlay_hints/parameter_hints/24_single_letter_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件中的转发包装函数**

头文件定义的包装函数与当前文件自身的包装函数一样，会解析出目标的参数名

编辑器会跳过文件顶部所包含头文件中的函数体，但最后一个参数是由自身模板参数构成的参数包的函数模板除外（如 `std::make_unique` 和 `emplace_back`）：它们转发到的调用就在该函数体中，提示显示的正是该调用的参数名。

```case
tests/inlay_hints/parameter_hints/27_forwarding_in_headers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型提示

类型由推导确定的声明会显示推导出的类型。

<!-- BEGIN GENERATED ITEMS: inlay_hints/type_hints -->

<!-- BEGIN CAPABILITY: supported -->

**推导的 `auto` 变量**

提示显示完整的变量类型，包含限定符

```case
tests/inlay_hints/type_hints/01_auto_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1298 clangd#1357 clangd#2268 -->

**类型语法糖与长度限制**

别名保留原有写法，过长的类型则保留放得下的部分

超出长度上限的类型会把放不下的模板实参写成 `…`，并保留最外层名称；连最外层名称都放不下的类型不显示任何内容。默认模板实参从不显示。上限由 `inlay_hints.max_length.types` 设置，设为 `0` 则取消限制。

```case
tests/inlay_hints/type_hints/02_type_sugar
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

每个绑定各自提示自身类型；聚合对象本身不显示提示

```case
tests/inlay_hints/type_hints/03_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1163 -->

**Lambda 表达式**

推导出的返回类型与初始化捕获会显示提示

保存 Lambda 的变量不显示类型：其初始化器已表明它是什么。

```case
tests/inlay_hints/type_hints/04_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导的返回类型**

推导出的返回类型以 `-> T` 的形式显示在参数列表之后

一旦后续定义给出了推导结果，声明处就会显示提示；没有定义的声明则不显示提示。已写出的尾置返回类型无需提示。

```case
tests/inlay_hints/type_hints/05_deduced_return_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 参数**

恰好只有一次实例化的模板会显示推导出的类型

第二次实例化会让推导产生歧义并隐去提示；参数包及其后的参数从不显示提示。多个实例化推导出相同类型时，模板体中只显示一次提示。

```case
tests/inlay_hints/type_hints/07_auto_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1749 clangd#2603 -->

**显式写出类型的初始化器**

初始化器中已写明类型的声明不显示提示

强制类型转换、函数式类型转换、`new`、以模板实参写出类型的工厂函数（如用 `make_owner<Box<int>>()` 构造 `Owner<Box<int>>`）以及 Lambda，都已表明变量保存的类型。若初始化器将类模板的实参留待推导，推导出的实参会直接显示在类模板名的书写位置；而结果类型含有调用处未写明的模板实参的工厂函数仍会显示提示。`inlay_hints.spelled_types` 会显示所有提示。

```case
tests/inlay_hints/type_hints/08_explicit_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2275 -->

**模板中的类型**

模板中的变量按模板自身的写法显示类型

取决于只有实例化才能算出的表达式（如 `input + 1`）的类型不显示任何内容，初始化器中已写明的类型也是如此。恰好只实例化一次的模板则改为显示该实例化中的类型。

```case
tests/inlay_hints/type_hints/09_dependent_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间**

提示按所在位置的代码所需的命名空间写出类型，但省略 `std`

类作用域会保留，`std` 内部的命名空间（`chrono::seconds`、`pmr::string`）同样保留；包围提示位置的命名空间，以及该处 using 指令引入的命名空间都会省去，匿名命名空间从不显示。模板实参遵循同样的规则。

```case
tests/inlay_hints/type_hints/10_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2275 -->

**已实例化的模板**

类模板内的 `auto` 变量会显示该模板唯一一次实例化所给出的类型

若两次实例化推导出不同类型，则按模板自身的写法显示类型。

```case
tests/inlay_hints/type_hints/12_instantiated_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**绑定中的别名**

容器元素的绑定保留声明该容器时使用的别名

遍历 `std::map<std::string, Point>` 时，每个键显示为 `const string`，而非 `basic_string<char>`。同一类型使用了两个不同别名，或同时使用了别名与原类型本身时，则不使用别名。

```case
tests/inlay_hints/type_hints/13_binding_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标准库名称**

标准库类型按标准规定的名称显示

标准库以别名命名的特化会显示该别名（如显示 `string` 而非 `basic_string<char>`），除非标准库为它起了两个别名；名称属于保留标识符的标准库内部别名，则改为显示它们所代表的类型。

```case
tests/inlay_hints/type_hints/16_standard_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**库的成员别名**

若类模板的成员别名所指代的类型更简短，则改为显示该类型

`vector<int>::size_type` 显示为 `size_t`，`vector<int>::reference` 显示为 `int &`，但 `vector<int>::iterator` 保持不变：它所指代的是标准库内部类型。命名空间声明的别名（`int64_t`、代码自定义的 `UserId`）或非模板类的成员别名（`steady_clock::time_point`）保持原名；仅代表内置类型的别名（`duration<long>::rep`）则显示该内置类型。

```case
tests/inlay_hints/type_hints/22_library_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标准库内部类型**

标准库留作内部使用的类型按其公开名称显示，没有公开名称则不显示

命名空间 `std` 或保留命名空间中的保留名称，以及带此类名称的类的成员，都属于标准库内部：`__alloc_traits<allocator<int>>::value_type &` 显示为 `int &`。算法返回的标准库内部类会显示为其实参或初始化器所属类中指代它的成员别名（以 `vector<int>::iterator` 表示 `__normal_iterator<int *, vector<int>>`）；没有任何公开名称指代的类型不显示提示。

```case
tests/inlay_hints/type_hints/23_library_internals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**计算得出的类型**

由类型特征（trait）计算出的类型显示为计算结果

类模板特化的成员 `type`（`common_type<seconds, seconds>::type`）、计算类型的别名模板（`enable_if_t<true, int>`）以及内置模板（`__type_pack_element`）都会替换为计算结果，并按代码中的写法命名：`chrono::seconds`、`int`。

```case
tests/inlay_hints/type_hints/24_computed_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基类的成员**

基类的成员别名按代码访问它时所通过的类来显示

`map.find(key)` 得到的是 `Map<int, long>::iterator`，而非其基类声明的 `MapBase<Map<int, long>, int, long>::iterator`。

```case
tests/inlay_hints/type_hints/25_base_member_aliases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 指派符提示

按位置进行聚合初始化时，会显示每个值所初始化的字段或索引。

<!-- BEGIN GENERATED ITEMS: inlay_hints/designator_hints -->

<!-- BEGIN CAPABILITY: supported clangd#1277 -->

**字段与索引指派符**

按位置进行聚合初始化时显示 `.field=`，启用后还会显示 `[index]=`

数组索引默认关闭，可通过 `inlay_hints.array_indices` 开启，否则数据表中的每个元素都会带上索引。未开启数组索引时，穿过数组的路径只显示其中的字段；开启数组索引但关闭字段指派符时，这类路径不显示任何内容。即使成员大小依赖于模板参数，数组指派符仍能正常显示；保留成员名则会跳过，不予显示。

```case
tests/inlay_hints/designator_hints/01_fields_and_indices
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套聚合体**

写出花括号时递归显示；省略花括号时展平为 `.outer.inner=`

```case
tests/inlay_hints/designator_hints/02_nested_aggregates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指派符提示抑制**

已写出的指派符和 `/*name=*/` 注释会使对应的初始化项不显示提示

将显式写出的指派符与按位置指定的初始化项混用是 C99 扩展，clang 接受这种写法但会给出警告；只有未加标注的 `4` 需要提示。

```case
tests/inlay_hints/designator_hints/04_suppression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**仅限聚合体**

构造函数调用、拷贝和惯用的零初始化不会生成指派符提示

花括号形式的构造函数调用改为显示构造函数的参数名，惯用的 `{}` 零初始化器则不显示提示。

```case
tests/inlay_hints/designator_hints/05_aggregates_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2540 -->

**圆括号聚合初始化**

C++20 的 `Point(1, 2)` 显示与花括号写法相同的指派符，即 `.x=` 和 `.y=`

```case
tests/inlay_hints/designator_hints/07_parenthesized_init
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 块结尾提示

十行及以上的代码块会在结尾注明它所结束的结构：默认针对命名空间和 `#endif`，开启 `inlay_hints.block_end` 后还包括函数、类型和控制流。

<!-- BEGIN GENERATED ITEMS: inlay_hints/block_end -->

<!-- BEGIN CAPABILITY: supported clangd#1634 -->

**右花括号后的名称**

较长代码块的右花括号后会显示提示，注明该花括号所结束的结构

代码块至少达到 `inlay_hints.min_block_lines` 行（默认 10 行，本例为 2 行）时，函数、类型和控制流都会获得提示。`if` 和 `while` 的条件较短时会显示条件，`else if` 链只显示为 `// if`，超过 60 个字符的标签用 `…` 截断。点击名称会跳转到对应代码块的开头。

```case
tests/inlay_hints/block_end/01_block_end_hints
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1807 -->

**代码块高度**

只有十行及以上的代码块才会在结尾获得提示

高度包含左右花括号所在的行。`inlay_hints.min_block_lines` 同时控制块结尾、命名空间结尾和 `#endif` 的最小行数。

```case
tests/inlay_hints/block_end/02_block_heights
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间结尾**

较长命名空间的右花括号后会注明其名称

`namespace a::b::c` 的结尾只有一条写出完整路径的提示，匿名命名空间只显示为 `// namespace`，后面已有注释的花括号不显示提示。默认开启。

```case
tests/inlay_hints/block_end/03_namespace_ends
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2487 -->

**`#endif` 后的条件**

结束较长条件编译块的 `#endif` 会显示它所结束的条件

`#ifdef NAME` 显示 `NAME`，`#ifndef NAME` 显示 `!NAME`，`#if` 按原样显示其条件，超过 60 个字符时用 `…` 截断，构建跳过的分支中也一样；后面已有注释的 `#endif` 不显示提示。点击条件会跳转到对应的 `#if`。默认开启。

```case
tests/inlay_hints/block_end/04_endif
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 默认实参提示

默认关闭（`inlay_hints.default_arguments`）。依赖默认实参的调用处会显示省略的实参，超出 `inlay_hints.max_length.default_arguments` 的部分会缩略显示。

<!-- BEGIN GENERATED ITEMS: inlay_hints/default_arguments -->

<!-- BEGIN CAPABILITY: supported -->

**省略的默认实参**

依赖默认实参的调用会显示所省略的实参，每个名称都链接到对应的参数

```case
tests/inlay_hints/default_arguments/01_omitted_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 可点击的提示

提示中的名称是指向其所指对象声明的链接，右花括号或 `#endif` 后的名称则链接到对应代码块的起始处。

<!-- BEGIN GENERATED ITEMS: inlay_hints/links -->

<!-- BEGIN CAPABILITY: supported clangd#1535 -->

**可点击的类型名**

类型提示中的每个类型名都链接到其声明：点击可跳转到定义，悬停可显示该类型的卡片

先声明后定义的类会链接到前置声明，再从该声明执行“跳转到定义”即可到达定义。模板实参逐个生成链接；内置类型和标点符号保持纯文本。

```case
tests/inlay_hints/links/02_type_links
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他提示种类

类模板实参推导（class template argument deduction）得出的模板实参。

<!-- BEGIN GENERATED ITEMS: inlay_hints/other_kinds -->

<!-- BEGIN CAPABILITY: supported clangd#2331 -->

**推导出的类模板实参**

实参由初始化过程推导出的类模板会显示这些实参

凡是只写出模板名而未写实参的地方——声明、函数式类型转换、`new` 表达式——实参都会紧跟在模板名之后，并按模板自身接受的形式显示，不受外围作用域影响，也不受推导指引（deduction guide）中所写别名或默认值的影响。由这类类型转换初始化的变量，只要模板实参已表明类型，就不再单独显示变量类型。

```case
tests/inlay_hints/other_kinds/02_ctad_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 配置

`[inlay_hints]` 配置节（位于 `clice.toml` 中，或通过 `initializationOptions` 传入同名键）包含每种提示的开关、代码块结尾显示提示的最小行数，以及 `[inlay_hints.max_length]` 下的长度上限。详见[配置指南](../guide/configuration.md#inlay-hints)。配置修改在重启服务器后生效。

## 交互行为

- 请求按范围限定：超出所请求范围的提示会被丢弃。
- 参数名提示锚定在实参左侧；类型提示和指派符提示锚定在声明一侧，使用 LSP 的间距标志代替内嵌空格。
- 完全相同的重复提示（例如来自多次模板实例化的提示）会合并为一条。
- 单行最多显示 100 条提示；更长行中超出的部分（例如写在同一行里的生成数据表）不会发送。
- 提示中的类型名、参数名和指派字段都是链接：点击可跳转到所指符号的定义，悬停可显示该符号的卡片。符号存在声明时，链接指向该声明，以便从该处执行“跳转到定义”到达定义。链接随提示一起返回；不支持 LSP 3.17 内联提示的客户端会收到纯文本。
