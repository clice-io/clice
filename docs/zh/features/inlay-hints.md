# 内联提示

<!-- The cards in the sections below are generated from the documented cases
     in tests/inlay_hints/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice 为代码中未显式给出的信息渲染内联标注：调用处的参数名、推导出的类型、按位置进行聚合初始化时对应的字段名，以及右花括号或 `#endif` 所结束的结构。每种提示在 `[inlay_hints]` 配置节中都有各自的开关；下面各节会说明哪些提示默认关闭。

## 参数名提示

调用处会显示每个实参所对应的参数名，除非实参本身已表明该名称。

<!-- BEGIN GENERATED ITEMS: inlay_hints/parameter_hints -->

<!-- BEGIN CAPABILITY: supported -->

**实参名称**

字面量实参会显示其对应的参数名

```case
tests/inlay_hints/parameter_hints/01_argument_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**提示抑制**

具名实参和 `/*name=*/` 注释会抑制参数名提示

实参与参数同名，或实参是未加限定的同名成员时，无需提示；限定名或通过对象访问的成员不算匹配。写有其他参数名的注释不会抑制任何提示。

```case
tests/inlay_hints/parameter_hints/03_suppression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1123 -->

**可修改引用标记**

用 `&` 标记通过非常量左值引用传递的实参

该标记有单独的开关 `inlay_hints.mutable_references`：关闭参数名提示后，该标记仍会保留，其后的参数名为空。

```case
tests/inlay_hints/parameter_hints/05_reference_markers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2324 -->

**转发解析**

经包装函数转发的参数包会解析出目标函数的参数名

直接转交实参时，无需 `std::forward` 也能解析出参数名；位于固定的前置与后置实参之间的参数包会解析到其对应的参数；传递给可调用对象的参数包则会解析到其调用运算符。最终用于初始化标量的参数包没有可显示的参数名。

```case
tests/inlay_hints/parameter_hints/06_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**来自定义的参数名**

声明中的未命名参数会采用定义中的参数名；参数名开头的下划线会去掉

声明和定义都为参数命名时，采用声明中的名称。

```case
tests/inlay_hints/parameter_hints/07_definition_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1734 clangd#1742 -->

**函数指针和调用运算符**

间接调用仍会显示参数名

```case
tests/inlay_hints/parameter_hints/08_function_objects
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**待决调用（dependent calls）**

即使只能在模板内部确定被调用者，也会显示参数名

根据实参数量匹配候选函数；只有筛选后仅剩一个候选函数时才显示参数名。若调用仍可能匹配多个重载，就不显示提示，也不猜测。

```case
tests/inlay_hints/parameter_hints/10_dependent_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2620 -->

**调用处的宏**

以宏形式书写的实参会显示提示；宏体内生成的调用不显示提示

类对象宏（object-like macro）在源码中算作一个实参。涵盖多个实参的宏没有可放置提示的位置；作为宏实参编写的代码会保留提示，其中嵌套了宏时也是如此，而嵌套宏自身的宏体不显示提示。

```case
tests/inlay_hints/parameter_hints/12_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2248 -->

**带前缀的参数名**

实参名与去掉单字母前缀后的参数名相同时，无需提示

传给 `aParam` 的 `param`、传给 `pBuffer` 的 `buffer` 都视为同名。`inlay_hints.prefixed_names` 会重新显示这些提示。

```case
tests/inlay_hints/parameter_hints/16_near_matches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1364 -->

**继承构造函数**

通过 `using Base::Base` 引入的构造函数，按基类声明中的写法显示参数名

```case
tests/inlay_hints/parameter_hints/17_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单字母参数名**

只有一个参数的函数不显示该参数的单字母名称

像 `push(int x)` 中的 `x` 这样的名称，并没有提供调用本身之外的信息；参数较多时，这些字母能区分各个参数，因此仍会显示。`inlay_hints.single_letter_names` 会显示所有这类名称。

```case
tests/inlay_hints/parameter_hints/24_single_letter_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型提示

如果声明的类型由推导确定，就会显示该类型。

<!-- BEGIN GENERATED ITEMS: inlay_hints/type_hints -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 变量的类型推导**

提示显示变量的完整类型，包括限定符

```case
tests/inlay_hints/type_hints/01_auto_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1298 clangd#1357 clangd#2268 -->

**类型语法糖的长度限制**

别名会保留原有写法，过长的类型则保留能放下的部分

超出长度上限的类型会先改用带语法糖的名称，再把放不下的模板实参写成 `…`，但保留最外层的名称；默认模板实参始终不显示。长度上限由 `inlay_hints.max_length.types` 设置；设为 `0` 即可取消限制。

```case
tests/inlay_hints/type_hints/02_type_sugar
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

每个绑定都显示其自身类型的提示；聚合对象本身不显示提示

```case
tests/inlay_hints/type_hints/03_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1163 -->

**Lambda**

推导出的返回类型和初始化捕获会显示提示

保存 Lambda 的变量不显示类型：其初始化器已表明它是什么。

```case
tests/inlay_hints/type_hints/04_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**返回类型推导**

推导出的返回类型以 `-> T` 的形式显示在参数列表之后

后续定义提供类型推导结果后，声明处就会显示提示；没有定义则不显示提示。已显式写出的尾置返回类型无需提示。

```case
tests/inlay_hints/type_hints/05_deduced_return_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 参数**

模板恰好只有一个实例时，会显示推导出的类型

第二次实例化会使推导产生歧义，并移除提示；参数包及其后的参数始终不显示提示。多个实例化推导出相同类型时，模板体中的提示只显示一次。

```case
tests/inlay_hints/type_hints/07_auto_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1749 clangd#2603 -->

**显式写出的初始化器**

初始化器已写出的类型不再显示提示

强制类型转换、函数式类型转换、`new`、以模板实参写出该类型的工厂函数（`make_owner<Box<int>>()` 得到 `Owner<Box<int>>`），以及 Lambda，都已表明变量保存的是什么。初始化器只写出类模板名、模板实参留待推导时，推导出的实参改为显示在类模板名的书写处；工厂函数的结果若带有调用中未写出的模板实参，仍会显示提示。`inlay_hints.spelled_types` 会显示所有提示。

```case
tests/inlay_hints/type_hints/08_explicit_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2275 -->

**模板中的类型**

模板中的变量按模板本身的写法显示其类型

类型取决于只有实例化才能算出的表达式（例如 `input + 1`）时，不显示提示；初始化器已写出的类型也不显示。恰好只实例化一次的模板则改为显示该实例化中的类型。

```case
tests/inlay_hints/type_hints/09_dependent_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域省略**

提示中省略命名空间限定符，保留类作用域

```case
tests/inlay_hints/type_hints/10_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2275 -->

**已实例化的模板**

类模板中的 `auto` 变量会显示该模板唯一实例化所给出的类型

当两个实例化推导出不同类型时，就没有一个统一的类型可供显示。

```case
tests/inlay_hints/type_hints/12_instantiated_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**绑定的别名**

容器元素的绑定会保留声明容器时使用的别名

遍历 `std::map<std::string, Point>` 时，每个键都显示为 `const std::string`，而不是 `basic_string<char>`。同一类型写成了两个别名，或者既写成别名又直接写出原类型时，提示不使用别名。

```case
tests/inlay_hints/type_hints/13_binding_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标准库名称**

标准库类型按标准为其规定的名称显示

标准库用别名命名的特化会显示为该别名，例如显示 `string` 而不是 `basic_string<char>`，但标准库为它起了两个别名时除外；标准库内部的别名使用保留名称，会改为显示其所代表的类型。

```case
tests/inlay_hints/type_hints/16_standard_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 指派符提示

按位置进行聚合初始化时，会显示每个值所初始化的字段或对应的索引。

<!-- BEGIN GENERATED ITEMS: inlay_hints/designator_hints -->

<!-- BEGIN CAPABILITY: supported clangd#1277 -->

**字段和索引指派符**

按位置进行聚合初始化时显示 `.field=`，启用后还会显示 `[index]=`

数组索引默认关闭，可通过 `inlay_hints.array_indices` 开启：否则数据表中的每个元素都会带上一个索引。关闭数组索引时，穿过数组的指派符路径只显示其中的字段；开启数组索引但关闭字段指派符时，这类路径不显示提示。即使成员的大小依赖于模板参数，数组指派符仍能正常显示；保留成员名则会被跳过，不予显示。

```case
tests/inlay_hints/designator_hints/01_fields_and_indices
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套聚合体**

显式写出花括号时递归处理；省略花括号时展平为 `.outer.inner=`

```case
tests/inlay_hints/designator_hints/02_nested_aggregates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指派符提示抑制**

已写出的指派符和 `/*name=*/` 注释会使对应的初始化器不再显示指派符提示

将显式写出的指派符与按位置指定的初始化器混用是 C99 扩展，clang 会接受这种写法并发出警告；只有未加标注的 `4` 需要提示。

```case
tests/inlay_hints/designator_hints/04_suppression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**仅限聚合体**

构造函数调用、拷贝和惯用的零初始化写法不产生指派符提示

使用花括号的构造函数调用会显示构造函数的形参名，而惯用的 `{}` 零初始化器则不显示提示。

```case
tests/inlay_hints/designator_hints/05_aggregates_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2540 -->

**圆括号聚合初始化**

C++20 的 `Point(1, 2)` 写法会显示与花括号写法相同的指派符，即 `.x=` 和 `.y=`

```case
tests/inlay_hints/designator_hints/07_parenthesized_init
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 块结尾提示

十行及以上的代码块会在结尾处注明它所结束的结构：默认只针对命名空间和 `#endif`，开启 `inlay_hints.block_end` 后还包括函数、类型和控制流结构。

<!-- BEGIN GENERATED ITEMS: inlay_hints/block_end -->

<!-- BEGIN CAPABILITY: supported clangd#1634 -->

**右花括号后的名称**

行数较多的代码块的右花括号后会显示提示，注明该花括号对应的结构

代码块至少有 `inlay_hints.min_block_lines` 行（默认十行，本例为两行）时，函数、类型和控制流结构都有这类提示。`if` 和 `while` 的条件较短时会显示条件，`else if` 链只显示为 `// if`，超过 60 个字符的标签会用 `…` 截断。点击名称会跳转到对应代码块的开头。

```case
tests/inlay_hints/block_end/01_block_end_hints
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1807 -->

**代码块高度**

只有十行及以上的代码块才会在结尾显示提示

高度包括左右花括号所在的行。`inlay_hints.min_block_lines` 同时为块结尾、命名空间结尾和 `#endif` 设置这一高度。

```case
tests/inlay_hints/block_end/02_block_heights
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间结尾**

行数较多的命名空间会在右花括号后注明其名称

`namespace a::b::c` 的结尾只有一条提示，写出完整路径；匿名命名空间只显示为 `// namespace`；后面已有注释的花括号不显示提示。默认开启。

```case
tests/inlay_hints/block_end/03_namespace_ends
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2487 -->

**`#endif` 后的条件**

结束较长条件编译块的 `#endif` 会显示它所结束的条件

`#ifdef NAME` 显示 `NAME`，`#ifndef NAME` 显示 `!NAME`，`#if` 则按原样显示其条件，超过 60 个字符时用 `…` 截断，构建时跳过的分支中也是如此；后面已有注释的 `#endif` 不显示提示。点击条件会跳转到对应的 `#if`。默认开启。

```case
tests/inlay_hints/block_end/04_endif
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 默认实参提示

默认关闭（`inlay_hints.default_arguments`）。使用默认实参的调用处会显示省略的实参，超出 `inlay_hints.max_length.default_arguments` 的内容会缩略显示。

<!-- BEGIN GENERATED ITEMS: inlay_hints/default_arguments -->

<!-- BEGIN CAPABILITY: supported -->

**省略的默认实参**

使用默认实参的调用会显示省略的实参，每个名称都链接到对应的形参

```case
tests/inlay_hints/default_arguments/01_omitted_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 可点击的提示

提示中的名称都链接到其所指对象的声明；右花括号或 `#endif` 后的名称则链接到对应代码块的开头。

<!-- BEGIN GENERATED ITEMS: inlay_hints/links -->

<!-- BEGIN CAPABILITY: supported clangd#1535 -->

**可点击的类型名**

类型提示中的每个类型名都链接到其声明：点击可跳转到定义，悬停可显示该类型的卡片

如果一个类在定义之前已有声明，则链接会指向该声明，再从该声明执行“跳转到定义”即可到达定义。模板实参各自带有链接；内置类型和标点符号仍为纯文本。

```case
tests/inlay_hints/links/02_type_links
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他提示类型

类模板实参推导（class template argument deduction）得出的模板实参。

<!-- BEGIN GENERATED ITEMS: inlay_hints/other_kinds -->

<!-- BEGIN CAPABILITY: supported clangd#2331 -->

**推导出的类模板实参**

类模板的实参由初始化推导得出时，会显示这些实参

凡是只写出模板名而未写实参的地方，实参都会显示在模板名之后：声明、函数式类型转换、`new` 表达式；实参按模板名本身接受的形式显示，不受外围作用域影响，也不受推导指引（deduction guide）中所写的别名或默认值影响。由这类类型转换初始化的变量，只要实参已经表明了类型，就不再单独显示类型。

```case
tests/inlay_hints/other_kinds/02_ctad_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 配置

`clice.toml` 的 `[inlay_hints]` 配置节（或通过 `initializationOptions` 提供的同名键）包含每种提示的开关、代码块结尾显示提示所需的最小高度，以及 `[inlay_hints.max_length]` 下的长度上限。详见[配置指南](../guide/configuration.md#inlay-hints)。配置更改会在服务器重启后生效，无需重新编译。

## 交互行为

- 请求按范围限定：请求范围之外的提示会被丢弃。
- 参数提示锚定在实参左侧；类型提示和指派符提示锚定在声明一侧，通过 LSP 的间距标志控制间距，而不嵌入空格。
- 内容完全相同的重复提示（例如模板实例化产生的提示）会合并为一条。
- 提示中的类型名、参数名和指派符字段都是链接：点击会跳转到所指符号的定义，悬停则显示该符号的悬停卡片。符号存在声明时，链接指向该声明，这样从链接处执行“跳转到定义”即可到达定义。链接随提示一起发给客户端；不支持 LSP 3.17 内联提示的客户端只收到纯文本。

## 其他已知不足

- [ ] 通过 `InlayHintLabelPart` 为缩写类型提示提供可展开的标签部分（[clangd#2269](https://github.com/clangd/clangd/issues/2269)）
