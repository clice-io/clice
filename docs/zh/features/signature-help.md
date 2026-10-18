# 签名帮助

## 重载签名

列出调用可能匹配的所有重载，并标记光标所在实参对应的参数。

<!-- BEGIN GENERATED ITEMS: signature_help/overload_signatures -->

<!-- BEGIN CAPABILITY: supported -->

**按参数个数区分的重载**

列出所有重载，并标记光标所在实参对应的参数

```case
tests/signature_help/overload_signatures/01_overloads_by_arity
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前参数**

在标签中标记光标所在实参绑定的参数

```case
tests/signature_help/overload_signatures/03_active_parameter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#50 -->

**const 重载**

参数相同的 const 与非 const 重载，只显示对象实际调用的那一个

非 const 对象调用非 const 重载；const 对象只能调用 const 重载，其标签以 `const` 结尾。

```case
tests/signature_help/overload_signatures/04_const_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**选定的重载**

用户在弹窗中切换到的重载，在继续输入调用时保持显示

在输入调用的过程中再次请求时，只要编辑器当前显示的重载仍在列表中，签名帮助就会保持它为当前重载，而不会跳回与实参最匹配的那一个。

```case
tests/signature_help/overload_signatures/07_kept_selection
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 调用场景

签名帮助会针对光标所在的调用显示，无论调用的是构造函数、函数指针、具有 `operator()` 的对象，还是模板。

<!-- BEGIN GENERATED ITEMS: signature_help/call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 -->

**构造函数与聚合类型**

调用构造函数时会列出构造函数，不显示返回类型；使用花括号进行聚合初始化时会列出字段

使用花括号书写时，构造函数也以花括号显示。

```case
tests/signature_help/call_contexts/01_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数指针调用**

通过函数指针调用时，会显示其类型声明中写出的参数名

```case
tests/signature_help/call_contexts/02_function_pointers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#299 -->

**模板实参列表**

模板参数会显示为签名，类模板的签名以其类别结尾，而不是返回类型

```case
tests/signature_help/call_contexts/03_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套调用**

在嵌套调用中，签名帮助会针对光标所在的最内层调用显示

```case
tests/signature_help/call_contexts/04_nested_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**仿函数调用**

调用对象时，会以对象的名称显示其 `operator()` 重载

调用中没有写出名称的对象（例如函数返回的对象）会显示为带限定符的 `operator()`。

```case
tests/signature_help/call_contexts/05_functors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#86 -->

**Lambda 调用**

调用 Lambda 时，以存放它的变量的名称显示其参数

就地调用的 Lambda 没有名称，因此只单独显示其参数。

```case
tests/signature_help/call_contexts/06_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1967 -->

**new 表达式**

new 表达式中的构造函数实参触发签名帮助

使用圆括号时，会列出构造函数；使用花括号时，目前还不会显示任何内容。

```case
tests/signature_help/call_contexts/07_new_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2472 -->

**下标运算符**

重载的 `operator[]` 目前还不提供签名帮助

```case
tests/signature_help/call_contexts/08_subscripts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1363 -->

**继承构造函数**

类的构造函数列表中缺少继承的构造函数

只显示隐式拷贝构造函数和移动构造函数，不显示由 `using Base::Base` 引入且调用实际使用的 `Base(int, int)`。

```case
tests/signature_help/call_contexts/09_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#795 -->

**函数式宏**

函数式宏的实参显示宏自身的参数，而不是其展开结果的参数

在宏实参中调用的函数显示其自身的签名。

```case
tests/signature_help/call_contexts/10_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板成员**

类模板的成员显示对象所属类实例化时使用的类型

`Vector<Point>::at` 返回 `Point &` 而不是 `T &`；通过函数对象调用时，会显示声明该对象时所用函数类型的参数。成员模板自身的参数则保持其声明中的写法。标准库类型以常用名称显示：`string` 而不是 `basic_string<char>`。

```case
tests/signature_help/call_contexts/13_class_template_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖成员调用**

在模板内部，通过依赖类型的对象发起的调用会显示其类模板声明的重载

过了第一个实参，就只保留还能再接受一个实参的重载。模板实例化之前，Clang 给不出这类调用的签名。

```case
tests/signature_help/call_contexts/17_dependent_member_calls
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 参数显示

每个参数按调用实际接受的形式显示，默认值则保持源码中的写法。

<!-- BEGIN GENERATED ITEMS: signature_help/parameters -->

<!-- BEGIN CAPABILITY: supported -->

**默认实参**

带有默认值的参数按源码中的写法显示其默认值

宏仍显示为宏，八进制数仍显示为八进制；跨多行书写的默认值合并为一行显示。

```case
tests/signature_help/parameters/01_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C 风格可变参数函数**

C 风格的 `...` 显示为最后一个参数，光标位于具名参数之后的任一实参上时都会标记它

```case
tests/signature_help/parameters/02_c_variadics
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**参数包**

参数包按模板声明中的写法显示，光标位于它接收的任一实参上时都会标记

```case
tests/signature_help/parameters/03_parameter_packs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2284 -->

**显式对象参数**

C++23 的 `this` 参数不会出现在标签中：调用成员函数时所用的对象不是实参

```case
tests/signature_help/parameters/05_explicit_object_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#517 clangd#638 -->

**转发函数**

将实参转发给构造函数的调用会显示用于转发的参数包，而非构造函数的形参

```case
tests/signature_help/parameters/06_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#638 -->

**标准库转发函数**

`make_unique`、`make_shared` 以及容器的 `emplace` 函数会显示其所构造对象的构造函数

列出能够接受目前已写出实参的构造函数，最匹配的排在最前；每个构造函数的参数都排在该函数自身的参数之后，例如 map 的 `try_emplace` 的键。聚合类型则列出其字段。

```case
tests/signature_help/parameters/07_standard_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**通过依赖特化访问的成员**

经由类模板的依赖特化调用的成员，按代码写出的模板实参显示

对于 `lib::vector<Point<T>>`，`pts.assign(` 的参数为 `const Point<T> &`，而不是模板自身的 `const _Tp &`。

```case
tests/signature_help/parameters/13_specialization_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文档

被调用函数的文档注释与其签名一同显示，格式为纯文本，读法与悬停相同；每个参数带有它的 `@param` 文本，编辑器会为正在填写的参数显示它。

<!-- BEGIN GENERATED ITEMS: signature_help/documentation -->

<!-- BEGIN CAPABILITY: supported -->

**文档注释**

被调用函数的文档注释与其签名一同显示，正在填写的参数旁显示它的 `@param` 文本

在 `[signature_help]` 节中设置 `documentation = false` 后，弹窗只显示签名。

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
