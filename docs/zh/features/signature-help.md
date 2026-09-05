# 签名帮助

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

- [ ] 使用花括号的 `new` 表达式应触发签名帮助（[clangd#1967](https://github.com/clangd/clangd/issues/1967)）

  ```cpp
  auto* w = new Widget{800, 600};
  //                   ^ should trigger signature help for Widget constructors
  ```

## 重载签名

<!-- BEGIN GENERATED ITEMS: overload_signatures -->

<!-- BEGIN CAPABILITY: supported -->

**函数重载**

签名帮助列出所有重载及其形参列表和返回类型

```snap
tests/snap/signature_help/overload_signatures/01_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前形参跟踪**

光标所在实参对应的形参用括号标出；此处光标位于第二个实参中

```snap
tests/snap/signature_help/overload_signatures/02_active_parameter.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员函数重载**

调用对象为非 const 时，同时列出 const 和非 const 重载；标签中不显示尾部的 const 限定符

```snap
tests/snap/signature_help/overload_signatures/03_member_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签中的默认实参**

带有默认实参的形参会在签名中显示其初始化器

```snap
tests/snap/signature_help/overload_signatures/04_default_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C 风格可变参数函数**

标签中列出具名形参，省略尾部的省略号

```snap
tests/snap/signature_help/overload_signatures/05_variadic_cstyle.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**可变参数模板的参数包**

参数包按被调用函数尚未实例化的签名显示

```snap
tests/snap/signature_help/overload_signatures/06_variadic_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前形参超出重载的形参范围**

光标位于第二个实参中时，仅保留声明了第二个形参的重载

```snap
tests/snap/signature_help/overload_signatures/07_active_beyond_last.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 模板实例化模式解析（显示模板模式，而非实例化结果）
- [ ] 过滤重复的 const/非 const 重载——只有一个可行时，不要两者都显示（[clangd#50](https://github.com/clangd/clangd/issues/50)）

  ```cpp
  struct Vec {
      int& operator[](size_t);
      const int& operator[](size_t) const;
  };
  Vec v;
  v[0];  // only show non-const overload (v is non-const)
  ```

- [ ] 优先显示用户提供的构造函数，而非编译器生成的构造函数（[clangd#1259](https://github.com/clangd/clangd/issues/1259)）

- [ ] 按参数个数过滤待决重载候选项（[clangd#2342](https://github.com/clangd/clangd/issues/2342)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo(1, 2);  // if T has foo(int) and foo(int,int), only show foo(int,int) as viable
  }
  ```

- [ ] 改进待决重载的启发式解析（[clangd#1083](https://github.com/clangd/clangd/issues/1083)）

- [ ] 从显示的签名中去除 C++23 显式对象形参（[clangd#2284](https://github.com/clangd/clangd/issues/2284)）

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show "(int x)", not "(this S& self, int x)"
  ```

## 特殊调用上下文

<!-- BEGIN GENERATED ITEMS: special_call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 clangd#2541 -->

**构造函数与聚合类型**

构造函数调用的签名不显示返回类型箭头；聚合初始化在花括号中列出字段

```snap
tests/snap/signature_help/special_call_contexts/01_constructor_aggregate.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数指针调用**

显示函数原型中的形参名称，而不仅是类型

```snap
tests/snap/signature_help/special_call_contexts/02_function_pointer.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#299 clangd#1387 -->

**模板实参列表**

模板形参作为签名显示；类模板的箭头指向其类别，而非返回类型

```snap
tests/snap/signature_help/special_call_contexts/03_template_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套调用**

在内层标记处显示内层调用的签名帮助，在外层标记处显示外层调用的签名帮助

```snap
tests/snap/signature_help/special_call_contexts/04_nested_calls.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数对象调用**

调用对象时，签名帮助显示其 operator() 重载

```snap
tests/snap/signature_help/special_call_contexts/05_operator_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 调用**

调用 Lambda 变量时，显示闭包的 operator() 形参

```snap
tests/snap/signature_help/special_call_contexts/06_lambda_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**new 表达式**

new 表达式中的构造函数实参触发签名帮助

```snap
tests/snap/signature_help/special_call_contexts/07_new_expression.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [ ] 继承构造函数——从派生类调用时显示基类构造函数（[clangd#1363](https://github.com/clangd/clangd/issues/1363)）

  ```cpp
  struct Base { Base(int x, int y); };
  struct Derived : Base { using Base::Base; };
  Derived d(^  // show Base(int x, int y)
  ```

- [ ] `operator[]` 签名帮助（[clangd#2472](https://github.com/clangd/clangd/issues/2472)）

  ```cpp
  std::map<std::string, int> m;
  m[^  // show operator[](const string& key)
  ```

- [ ] Lambda 调用——显示 Lambda 名称而非 `operator()`（[clangd#86](https://github.com/clangd/clangd/issues/86)）

  ```cpp
  auto validate = [](int x, int max) -> bool { ... };
  validate(^  // show "validate(int x, int max) -> bool", not "operator()(int x, int max)"
  ```

- [ ] 函数指针调用——显示参数名（[clangd#1068](https://github.com/clangd/clangd/issues/1068)、[clangd#1729](https://github.com/clangd/clangd/issues/1729)）

  ```cpp
  void (*callback)(int status, const char* msg);
  callback(^  // show "(int status, const char* msg)"
  ```

- [ ] 对象初始化时的构造函数签名帮助

- [ ] 函数式宏调用——显示宏参数，而非底层展开内容（[clangd#795](https://github.com/clangd/clangd/issues/795)）

  ```cpp
  #define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)
  CHECK(^  // show "CHECK(cond, msg)", not "fail(const char*)"
  ```

## 参数显示

- [ ] 转发函数参数解析——为 `std::make_unique`、`emplace_back` 等显示底层构造函数的参数（[clangd#517](https://github.com/clangd/clangd/issues/517)）

  ```cpp
  struct Widget { Widget(int width, int height); };
  std::make_unique<Widget>(^  // show "(int width, int height)"
  ```

- [ ] 参数包显示（[clangd#638](https://github.com/clangd/clangd/issues/638)）

  ```cpp
  template<typename... Args>
  void log(const char* fmt, Args&&... args);
  log("x=%d y=%d", ^  // show "fmt, args..." with active parameter on args
  ```

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

- [ ] 显示默认参数值

  ```cpp
  void open(std::string path, int mode = 0644);
  open("file", ^  // show "int mode = 0644" (active), user knows it can be omitted
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
