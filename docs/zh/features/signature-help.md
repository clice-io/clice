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

<!-- END GENERATED ITEMS -->

- [x] 模板实例化模式解析（显示模板模式，而非实例化结果）

- [ ] 优先显示用户提供的构造函数，而非编译器生成的构造函数（[clangd#1259](https://github.com/clangd/clangd/issues/1259)）

- [ ] 按参数个数过滤待决重载候选项（[clangd#2342](https://github.com/clangd/clangd/issues/2342)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo(1, 2);  // if T has foo(int) and foo(int,int), only show foo(int,int) as viable
  }
  ```

- [ ] 改进待决重载的启发式解析（[clangd#1083](https://github.com/clangd/clangd/issues/1083)）

## 调用场景

签名帮助会针对光标所在的调用显示，无论调用的是构造函数、函数指针、具有 `operator()` 的对象，还是模板。

<!-- BEGIN GENERATED ITEMS: signature_help/call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 clangd#2541 -->

**构造函数与聚合类型**

调用构造函数时会列出构造函数，不显示返回类型；使用花括号进行聚合初始化时会列出字段

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

<!-- BEGIN CAPABILITY: supported clangd#299 clangd#1387 -->

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

调用对象时会显示其 `operator()` 重载

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

<!-- END GENERATED ITEMS -->

## 参数显示

每个参数均按声明中的写法显示，并包含默认值。

<!-- BEGIN GENERATED ITEMS: signature_help/parameters -->

<!-- BEGIN CAPABILITY: supported -->

**默认实参**

带有默认值的参数会在标签中显示其默认值

```case
tests/signature_help/parameters/01_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**C 风格可变参数函数**

会列出具名参数，但标签省略了末尾的 `...`

光标移到具名参数之后的实参上时，不会标记任何参数，标签中也没有提示该函数还能接受更多实参。

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

<!-- BEGIN CAPABILITY: unsupported clangd#2284 -->

**显式对象参数**

C++23 的 `this` 参数会显示在标签中，且光标位于第一个实参上时会标记该参数

调用成员函数时所用的对象不是实参，但标签仍会列出 `this Shape &self`，并在光标位于 `dx` 对应的实参上时标记它。

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

<!-- END GENERATED ITEMS -->

- [ ] 美化标准库参数名（[clangd#736](https://github.com/clangd/clangd/issues/736)）

  ```
  // current:  push_back(const value_type& __x)
  // expected: push_back(const value_type& value)
  ```

- [ ] 保留参数类型中的枚举类作用域（[clangd#2475](https://github.com/clangd/clangd/issues/2475)）

  ```cpp
  enum class Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // show "(Color c)", not "(c)" with scope stripped
  ```

## 文档

- [ ] 当前参数的文档（来自 `@param` 文档注释）

  ```cpp
  /// @param path The file system path.
  /// @param mode POSIX file permission bits.
  void open(std::string path, int mode);
  open("file", ^  // show documentation for mode parameter
  ```

- [ ] 遵循 `documentationFormat` 能力（[clangd#945](https://github.com/clangd/clangd/issues/945)）
- [ ] 通过继承的构造函数传递文档（[clangd#1936](https://github.com/clangd/clangd/issues/1936)）
- [ ] 重载集中的重载数量指示器

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

- [ ] 避免误触发——不要在注释、字符串字面量中或定义函数时触发（[clangd#51](https://github.com/clangd/clangd/issues/51)、[clangd#289](https://github.com/clangd/clangd/issues/289)）

  ```cpp
  void foo(int x, int y) {  // should NOT trigger signature help
  //       ^^^^^^^^^^^^^ this is a definition, not a call
  ```
