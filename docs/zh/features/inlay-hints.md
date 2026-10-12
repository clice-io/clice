# 内联提示

<!-- The cards in the sections below are generated from the documented cases
     in tests/inlay_hints/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice 为代码中未显式给出的信息渲染内联标注：调用处的参数名、推导出的类型，以及按位置进行聚合初始化时对应的字段名。提示类别可以通过 `[inlay_hints]` 配置节单独开关；下面各节介绍默认开启的类别。

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

<!-- BEGIN CAPABILITY: supported clangd#1877 -->

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

```case
tests/inlay_hints/parameter_hints/05_reference_markers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2324 -->

**转发解析**

经包装函数转发的参数包会解析出目标函数的参数名

直接转交实参时，无需 `std::forward` 也能解析出参数名；位于固定的前置与后置实参之间的参数包会解析到其对应的参数；传递给可调用对象的参数包则会解析到其调用运算符。

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

类对象宏（object-like macro）在源码中算作一个实参。涵盖多个实参的宏没有可放置提示的位置，而作为宏实参编写的代码会保留提示。

```case
tests/inlay_hints/parameter_hints/12_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2248 -->

**宽松名称匹配**

参数名 `aParam` 目前还无法抑制实参 `param` 的提示

```case
tests/inlay_hints/parameter_hints/16_near_matches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1364 -->

**继承构造函数**

调用通过 `using Base::Base` 继承的构造函数时，参数名会丢失

```case
tests/inlay_hints/parameter_hints/17_inherited_constructors
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

<!-- BEGIN CAPABILITY: supported clangd#1298 clangd#1357 -->

**类型语法糖的长度限制**

别名会保留原有写法，类型名过长时则改用带语法糖的名称

如果类型没有足够短的写法，就不显示提示；默认模板实参始终不显示。长度上限由 `inlay_hints.type_name_limit` 设置；设为 `0` 即可取消限制。

```case
tests/inlay_hints/type_hints/02_type_sugar
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

每个绑定都显示其规范类型的提示；聚合对象本身不显示提示

```case
tests/inlay_hints/type_hints/03_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1163 -->

**Lambda**

变量、推导出的返回类型和初始化捕获都会显示提示

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

<!-- BEGIN CAPABILITY: partial clangd#1749 -->

**显式写出的初始化器**

即使初始化器已在强制类型转换或函数式类型转换中写出类型，仍会显示该类型的提示

```case
tests/inlay_hints/type_hints/08_explicit_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域省略**

提示中省略命名空间限定符，保留类作用域

```case
tests/inlay_hints/type_hints/10_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2275 -->

**已实例化的模板**

模板体中的提示只显示一次，但类型依赖于模板的变量不会显示提示，即使唯一的实例化已确定其类型也是如此

当两个实例化推导出不同类型时，就没有一个统一的类型可供显示。

```case
tests/inlay_hints/type_hints/12_instantiated_templates
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 指派符提示

按位置进行聚合初始化时，会显示每个值所初始化的字段或对应的索引。

<!-- BEGIN GENERATED ITEMS: inlay_hints/designator_hints -->

<!-- BEGIN CAPABILITY: supported clangd#2303 -->

**字段和索引指派符**

按位置进行聚合初始化时显示 `.field=` 和 `[index]=`

即使成员的大小依赖于模板参数，数组指派符仍能正常显示；保留成员名则会被跳过，不予显示。

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

<!-- BEGIN CAPABILITY: unsupported clangd#2540 -->

**圆括号聚合初始化**

C++20 的 `Point(1, 2)` 写法目前还没有提示

```case
tests/inlay_hints/designator_hints/07_parenthesized_init
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 块结尾提示

默认关闭（`inlay_hints.block_end`）。跨行代码块的右花括号后会显示提示，注明该花括号对应的结构。

<!-- BEGIN GENERATED ITEMS: inlay_hints/block_end -->

<!-- BEGIN CAPABILITY: supported -->

**右花括号后的名称**

跨行代码块的右花括号后会显示提示，注明该花括号对应的结构

函数、类型、命名空间和控制流结构都有这类提示；`if` 和 `while` 的条件较短时会显示条件，`else if` 链只显示为 `// if`，超过 60 个字符的标签则会省略。单行代码块不显示提示。

```case
tests/inlay_hints/block_end/01_block_end_hints
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

一个相关设想是用 `#endif` 提示显示与之匹配的条件（[clangd#2487](https://github.com/clangd/clangd/issues/2487)），但该功能尚未实现。

## 默认实参提示

默认关闭（`inlay_hints.default_arguments`）。使用默认实参的调用处会显示省略的实参，超出类型名长度限制的内容会缩略显示。

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

提示中的名称都链接到其所指对象的声明。

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

代码中隐含的其他信息目前还没有提示。

<!-- BEGIN GENERATED ITEMS: inlay_hints/other_kinds -->

<!-- BEGIN CAPABILITY: unsupported clangd#2583 -->

**模板实参名称**

调用处显式指定的模板实参，目前还不会显示它所绑定的模板形参

```case
tests/inlay_hints/other_kinds/01_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2331 -->

**推导出的类模板实参**

对于通过构造函数调用推导出实参的类模板，目前还不会显示这些实参

```case
tests/inlay_hints/other_kinds/02_ctad_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2254 -->

**隐式转换**

调用处的实参发生隐式转换时，目前还不会显示该转换

```case
tests/inlay_hints/other_kinds/03_implicit_conversions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 配置

`clice.toml` 的 `[inlay_hints]` 配置节（或通过 `initializationOptions` 提供的同名键）控制所有类别：`enabled`、`parameters`、`deduced_types`、`designators`、`block_end`、`default_arguments` 和 `type_name_limit`。详见[配置指南](../guide/configuration.md#inlay-hints)。配置更改会在服务器重启后生效，无需重新编译。

## 交互行为

- 请求按范围限定：请求范围之外的提示会被丢弃。
- 参数提示锚定在实参左侧；类型提示和指派符提示锚定在声明一侧，通过 LSP 的间距标志控制间距，而不嵌入空格。
- 内容完全相同的重复提示（例如模板实例化产生的提示）会合并为一条。
- 提示中的类型名、参数名和指派符字段都是链接：点击会跳转到所指符号的定义，悬停则显示该符号的悬停卡片。符号存在声明时，链接指向该声明，这样从链接处执行“跳转到定义”即可到达定义。链接随提示一起发给客户端；不支持 LSP 3.17 内联提示的客户端只收到纯文本。

## 其他已知不足

- [ ] 通过 `InlayHintLabelPart` 为缩写类型提示提供可展开的标签部分（[clangd#2269](https://github.com/clangd/clangd/issues/2269)）
- [ ] 根据作用域缩写类型名——在 `namespace foo` 内显示 `Bar` 而不是 `foo::Bar`（[clangd#2270](https://github.com/clangd/clangd/issues/2270)）
- [ ] 协程返回模板类型时丢失参数提示（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）
