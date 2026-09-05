# 内联提示

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/inlay_hint/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 为代码中未显式给出的信息渲染内联标注：调用处的参数名、推导出的类型，以及按位置进行聚合初始化时对应的字段名。提示类别可以通过 `[inlay_hints]` 配置节单独开关；下面各节介绍默认开启的类别。

## 参数名提示

<!-- BEGIN GENERATED ITEMS: parameter_hints -->

<!-- BEGIN CAPABILITY: supported -->

**参数名提示**

在函数和构造函数的调用处显示参数名

```snap
tests/snap/inlay_hint/parameter_hints/01_param_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1877 -->

**提示抑制**

具名实参和 `/*name=*/` 注释会抑制参数名提示

```snap
tests/snap/inlay_hint/parameter_hints/02_param_suppression.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**设置函数和内置函数的提示抑制**

`setX(x)` 和 `std::move`/`std::forward` 的实参不显示提示

```snap
tests/snap/inlay_hint/parameter_hints/03_param_setters_builtins.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1123 -->

**可修改引用标记**

用 `&` 标记通过非常量左值引用传递的实参

```snap
tests/snap/inlay_hint/parameter_hints/04_param_references.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2324 -->

**转发解析**

经包装函数转发的参数包会解析出目标函数的参数名

```snap
tests/snap/inlay_hint/parameter_hints/05_param_forwarding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**来自定义的参数名**

声明中的未命名参数使用定义中的参数名，并去掉开头的下划线

```snap
tests/snap/inlay_hint/parameter_hints/06_param_definition_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1734 clangd#1742 -->

**函数指针和调用运算符**

间接调用仍会显示参数名

```snap
tests/snap/inlay_hint/parameter_hints/07_param_function_objects.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1777 -->

**推导 `this`**

显式对象参数从不显示提示（C++23）

```snap
tests/snap/inlay_hint/parameter_hints/08_param_deducing_this.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**待决调用（dependent calls）**

即使只能在模板内部确定被调用者，也会显示参数名

根据实参数量筛选候选函数；只有筛选后剩下唯一候选函数时，才会显示参数名。因此，如果调用仍可能匹配多个重载，就不显示提示，以免猜测。

```snap
tests/snap/inlay_hint/parameter_hints/09_param_dependent.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未展开的参数包**

代码中显式写出的参数包展开会打破实参与形参的一一对应关系，停止显示提示

```snap
tests/snap/inlay_hint/parameter_hints/10_param_packs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2620 -->

**调用处的宏**

以宏形式书写的实参会显示提示；宏体内生成的调用不显示提示

```snap
tests/snap/inlay_hint/parameter_hints/11_param_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**隐式构造函数调用**

代码中未显式写出的转换本身不会产生提示

```snap
tests/snap/inlay_hint/parameter_hints/12_param_implicit_conversions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**伪对象表达式（pseudo-object expressions）**

MS 属性访问不显示提示；显式写出的下标操作仍会显示访问器的参数名

```snap
tests/snap/inlay_hint/parameter_hints/13_param_pseudo_objects.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1034 -->

**显式实例化**

显式实例化定义不会添加重复提示，其中显式写出的模板实参仍正常显示提示

```snap
tests/snap/inlay_hint/parameter_hints/14_param_explicit_instantiation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2248 -->

**宽松名称匹配**

参数名 `aParam` 目前还无法抑制实参 `param` 的提示

```snap
tests/snap/inlay_hint/parameter_hints/15_param_case_insensitive.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1364 -->

**继承构造函数**

调用通过 `using Base::Base` 继承的构造函数时，参数名会丢失

```snap
tests/snap/inlay_hint/parameter_hints/16_param_inherited_constructors.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未命名参数**

未命名参数不显示参数名提示，但可修改引用仍会显示 `&`

```snap
tests/snap/inlay_hint/parameter_hints/17_param_anonymous.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**运算符和字面量**

运算符语法和用户定义字面量不显示提示；成员初始化器和默认成员初始化器会显示提示

```snap
tests/snap/inlay_hint/parameter_hints/18_param_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**构造函数实参中的参数包**

外层调用可以解析；展开内部的提示仍然缺失

```snap
tests/snap/inlay_hint/parameter_hints/19_param_pack_constructors.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 类型提示

<!-- BEGIN GENERATED ITEMS: type_hints -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 变量的类型推导**

提示显示变量的完整类型，包括限定符

```snap
tests/snap/inlay_hint/type_hints/01_type_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1298 clangd#1357 -->

**类型语法糖的长度限制**

别名保留原写法；类型过长时，改用保留语法糖的名称

```snap
tests/snap/inlay_hint/type_hints/02_type_sugar.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

每个绑定都显示其规范类型的提示；聚合对象本身不显示提示

```snap
tests/snap/inlay_hint/type_hints/03_type_structured_bindings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1163 -->

**Lambda**

变量、推导出的返回类型和初始化捕获都会显示提示

```snap
tests/snap/inlay_hint/type_hints/04_type_lambdas.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**返回类型推导**

推导出的返回类型以 `-> T` 的形式显示在参数列表之后

```snap
tests/snap/inlay_hint/type_hints/05_type_auto_return.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` 写法**

在显式写出的 `decltype` 旁显示其实际类型

```snap
tests/snap/inlay_hint/type_hints/06_type_decltype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` 参数**

模板恰好只有一个实例时，会显示推导出的类型

```snap
tests/snap/inlay_hint/type_hints/07_type_auto_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1749 -->

**显式写出的初始化器**

类型转换和函数式类型转换仍会显示多余的提示

```snap
tests/snap/inlay_hint/type_hints/08_type_explicit_source.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2275 -->

**待决 `auto`**

未实例化的模板体内不显示推导提示

```snap
tests/snap/inlay_hint/type_hints/09_type_dependent.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域省略**

提示中省略命名空间限定符，保留类作用域

```snap
tests/snap/inlay_hint/type_hints/10_type_scopes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基于元组协议的绑定**

提示显示规范类型，而非 `tuple_element<I, T>::type`

```snap
tests/snap/inlay_hint/type_hints/11_type_bindings_tuple.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2275 -->

**已实例化的模板**

实例化后的模板体不会在模板原始定义处重复显示提示；对于依赖模板参数的 `auto`，
在恰好只有一个实例化时可显示推导出的类型

```snap
tests/snap/inlay_hint/type_hints/12_type_conflicting_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 指派符提示

<!-- BEGIN GENERATED ITEMS: designator_hints -->

<!-- BEGIN CAPABILITY: supported clangd#2303 -->

**字段和索引指派符**

按位置进行聚合初始化时显示 `.field=` 和 `[index]=`

```snap
tests/snap/inlay_hint/designator_hints/01_designator_basic.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套聚合体**

显式写出花括号时递归处理；省略花括号时展平为 `.outer.inner=`

```snap
tests/snap/inlay_hint/designator_hints/02_designator_nested.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**匿名成员**

指派符路径中省略匿名联合体和结构体

```snap
tests/snap/inlay_hint/designator_hints/03_designator_anonymous.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指派符提示抑制**

已写出的指派符和 `/*name=*/` 注释会使对应的初始化器不再显示指派符提示

```snap
tests/snap/inlay_hint/designator_hints/04_designator_suppression.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**仅限聚合体**

构造函数调用、拷贝和惯用的零初始化写法不产生指派符提示

```snap
tests/snap/inlay_hint/designator_hints/05_designator_aggregates_only.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**有误的初始化器**

无法通过编译的初始化器旁仍会显示指派符提示

```snap
tests/snap/inlay_hint/designator_hints/06_designator_recovery.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2540 -->

**圆括号聚合初始化**

C++20 的 `Point(1, 2)` 写法目前还没有提示

```snap
tests/snap/inlay_hint/designator_hints/07_designator_parenthesized.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他提示类型

<!-- BEGIN GENERATED ITEMS: other_hint_kinds -->

<!-- BEGIN CAPABILITY: unsupported clangd#2583 -->

**模板参数提示**

目前尚未在调用点显示模板实参提示

```snap
tests/snap/inlay_hint/other_hint_kinds/01_template_parameter_hints.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2331 -->

**CTAD 实参**

CTAD 目前尚不显示推导出的类模板实参

```snap
tests/snap/inlay_hint/other_hint_kinds/02_ctad_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2254 -->

**隐式转换提示**

调用点的隐式转换目前还没有提示

```snap
tests/snap/inlay_hint/other_hint_kinds/03_conversion_hints.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 块结尾提示

默认关闭（`inlay_hints.block_end`）。对于至少跨两行的代码块，clice 会在右花括号后显示其所结束结构的名称——包括函数、类型、命名空间和控制流语句：

```cpp
void Widget::process(const Config& cfg) {
    // ...
} // Widget::process

namespace detail {
    // ...
} // namespace detail

while (running) {
    // ...
} // while running
```

如果条件能以简短文本表示，就会为 `if`/`while`/`switch`/`for` 显示条件摘要；`else if` 链仅提示为 `// if`。超过 60 个字符的标签不会显示。

一个相关设想是用 `#endif` 提示显示与之匹配的条件（[clangd#2487](https://github.com/clangd/clangd/issues/2487)），但该功能尚未实现。

## 默认实参提示

默认关闭（`inlay_hints.default_arguments`）。依赖默认实参的调用点会显示省略的内容，超过类型名长度限制时会缩写：

```cpp
void log(int level, bool flush = true, int repeat = 1);
log(2);
//     ^ , flush: true, repeat: 1
```

## 配置

`clice.toml` 的 `[inlay_hints]` 配置节（或通过 `initializationOptions` 提供的同名键）控制所有类别：`enabled`、`parameters`、`deduced_types`、`designators`、`block_end`、`default_arguments` 和 `type_name_limit`。详见[配置指南](../guide/configuration.md#inlay-hints)。配置更改会在服务器重启后生效，无需重新编译。

## 交互行为

- 请求按范围限定：请求范围之外的提示会被丢弃。
- 参数提示锚定在实参左侧；类型提示和指派符提示锚定在声明一侧，通过 LSP 的间距标志控制间距，而不嵌入空格。
- 内容完全相同的重复提示（例如模板实例化产生的提示）会合并为一条。

## 其他已知不足

- [ ] 通过 `InlayHintLabelPart` 为缩写类型提示提供可展开的标签部分（[clangd#2269](https://github.com/clangd/clangd/issues/2269)）
- [ ] 可点击的类型名——在提示的类型上跳转到定义（[clangd#1535](https://github.com/clangd/clangd/issues/1535)）
- [ ] 根据作用域缩写类型名——在 `namespace foo` 内显示 `Bar` 而不是 `foo::Bar`（[clangd#2270](https://github.com/clangd/clangd/issues/2270)）
- [ ] 协程返回模板类型时丢失参数提示（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）
