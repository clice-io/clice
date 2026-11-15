# 签名帮助

## 重载签名

函数调用会列出可能命中的全部重载，并高亮标出光标所在实参对应的形参。

<!-- BEGIN GENERATED ITEMS: signature_help/overload_signatures -->

<!-- BEGIN CAPABILITY: supported -->

**按参数个数区分的重载**

列出全部重载，并标出光标所在实参对应的形参

```case
tests/signature_help/overload_signatures/01_overloads_by_arity
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前形参**

在签名标签中标出光标所在实参绑定的形参

```case
tests/signature_help/overload_signatures/03_active_parameter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#50 -->

**const 重载**

形参相同的 const 与非 const 重载中，只显示当前对象实际调用的那一个

非 const 对象调用非 const 重载；const 对象只能调用 const 重载，其签名标签以 `const` 结尾。

```case
tests/signature_help/overload_signatures/04_const_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**选定的重载**

用户在弹窗中手动选定的重载在继续输入实参时保持激活

在输入调用实参的过程中再次触发请求时，只要编辑器当前展示的重载仍在候选列表中，签名帮助就会继续保持它处于激活状态，而不会跳回最匹配当前实参的那个重载。

```case
tests/signature_help/overload_signatures/07_kept_selection
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用场景

签名帮助会跟随光标外围的调用展示，无论被调用的是构造函数、函数指针、带有 `operator()` 的对象还是模板。

<!-- BEGIN GENERATED ITEMS: signature_help/call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 -->

**构造函数与聚合类型**

构造函数调用列出不含返回类型的各个构造函数，花括号聚合初始化则列出各个字段

以花括号书写时，构造函数签名也以花括号展示。

```case
tests/signature_help/call_contexts/01_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数指针调用**

通过函数指针发起的调用显示其函数类型写出的形参名

```case
tests/signature_help/call_contexts/02_function_pointers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#299 -->

**模板实参列表**

模板参数作为签名展示，类模板末尾显示其声明类别而非返回类型

```case
tests/signature_help/call_contexts/03_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套调用**

嵌套调用中，签名帮助跟随包围光标的最内层调用

```case
tests/signature_help/call_contexts/04_nested_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数对象调用**

调用对象时以该对象的名称展示其 `operator()` 重载

调用处未命名的对象（如函数返回的临时对象）则显示带限定符的 `operator()`。

```case
tests/signature_help/call_contexts/05_functors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#86 -->

**Lambda 调用**

调用 Lambda 时以持有它的变量名展示其形参

在定义处立即调用的 Lambda 没有名称，因此只单独显示形参列表。

```case
tests/signature_help/call_contexts/06_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1967 -->

**new 表达式**

new 表达式中的构造函数实参可触发签名帮助

以圆括号书写时会列出构造函数；以花括号书写时则不显示任何内容。

```case
tests/signature_help/call_contexts/07_new_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2472 -->

**下标运算符**

重载的 `operator[]` 不显示签名帮助

```case
tests/signature_help/call_contexts/08_subscripts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1363 -->

**继承的构造函数**

类继承来的构造函数未出现在构造函数列表中

只会显示隐式复制与移动构造函数，而不会显示调用实际使用的 `Base(int, int)`（由 `using Base::Base` 引入）。

```case
tests/signature_help/call_contexts/09_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#795 -->

**函数式宏**

函数式宏的实参处显示宏自身的形参，而非展开后目标的形参

在宏实参内部调用的函数则显示该函数自身的签名。

```case
tests/signature_help/call_contexts/10_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板成员**

类模板的成员按对象所属类实例化后的具体类型显示

`Vector<Point>::at` 的返回类型显示为 `Point &` 而非 `T &`；通过函数对象发起的调用则显示声明该对象时所用函数类型的形参。成员模板自身的模板参数保留其声明写法。标准库类型按常用别名展示：显示 `string`，而非 `basic_string<char>`。

```case
tests/signature_help/call_contexts/13_class_template_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖成员调用**

在模板内部，通过依赖类型对象发起的调用显示其类模板声明的重载

填过第一个实参后，只保留还能继续接受实参的重载。在模板实例化之前，Clang 本身无法为这类调用给出签名。

```case
tests/signature_help/call_contexts/17_dependent_member_calls
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 参数显示

每个形参按调用实际接收的形式展示，默认值保留源码中的写法。

<!-- BEGIN GENERATED ITEMS: signature_help/parameters -->

<!-- BEGIN CAPABILITY: supported -->

**默认实参**

带默认值的形参按源码写法展示其默认值

宏保留为宏名，八进制数保留为八进制写法；跨多行书写的默认值合并为单行展示。

```case
tests/signature_help/parameters/01_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C 风格可变参数函数**

C 风格的 `...` 显示为最后一个形参，光标位于具名形参之后的任一实参上时均标出它

```case
tests/signature_help/parameters/02_c_variadics
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**参数包**

参数包按模板声明中的写法显示，光标位于它接收的任一实参上时均标出它

```case
tests/signature_help/parameters/03_parameter_packs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2284 -->

**显式对象形参**

C++23 的 `this` 形参不出现在签名标签中：调用成员函数所用的对象并非调用实参

```case
tests/signature_help/parameters/05_explicit_object_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#517 clangd#638 -->

**转发函数**

将实参转发给构造函数的调用显示目标构造函数的形参

在定义体中用末尾参数包构造类的函数模板——如 `new T(args...)`、`T(args...)`、`T{args...}`、以此方式初始化的变量，或 `std::make_unique<T>(args...)`——会展示目标类的各个构造函数，每个构造函数的形参接在该函数自身位于参数包之前的形参之后。若该函数本身是目标类的成员，私有构造函数也会列出。若参数包用于构造由各实参自身类型拼出的类型，则按原声明展示。

```case
tests/signature_help/parameters/06_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#638 -->

**标准库转发函数**

`make_unique`、`make_shared` 以及容器的 `emplace` 系列函数展示所构造类型的构造函数

列出能够接受当前已写实参的构造函数，最佳匹配排在最前，每个构造函数的形参都排在函数自身形参（如 map 的 `try_emplace` 的键形参）之后。聚合类型则列出其各个字段。

```case
tests/signature_help/parameters/07_standard_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过依赖特化调用的成员**

通过类模板的依赖特化调用成员时，按代码写出的模板实参展示类型

调用 `pts.assign(` 时，若接收方类型为 `lib::vector<Point<T>>`，形参会显示为 `const Point<T> &`，而非模板内部声明的 `const _Tp &`。

```case
tests/signature_help/parameters/13_specialization_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档

被调用函数的文档注释随签名一同展示，格式为纯文本，解析方式与悬停一致；每个形参附带各自的 `@param` 说明，由编辑器在填写该形参时展示。

<!-- BEGIN GENERATED ITEMS: signature_help/documentation -->

<!-- BEGIN CAPABILITY: supported -->

**文档注释**

被调用函数的文档注释随签名展示，正在填写的形参附带对应的 `@param` 说明

设置 `documentation = false`（位于 `[signature_help]` 配置节）后，弹窗只展示签名。

```case
tests/signature_help/documentation/01_doc_comments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 触发字符

已注册：`(`、`)`、`{`、`}`、`<`、`>`、`,`

| 字符 | 上下文       | 行为         |
| ---- | ------------ | ------------ |
| `(`  | 函数调用     | 显示重载签名 |
| `)`  | 右圆括号     | 更新上下文   |
| `{`  | 花括号初始化 | 显示重载签名 |
| `}`  | 右花括号     | 更新上下文   |
| `<`  | 模板实参     | 显示重载签名 |
| `>`  | 模板闭合     | 更新上下文   |
| `,`  | 实参分隔符   | 更新当前形参 |
