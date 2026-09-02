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

- [ ] 避免误触发——不要在注释、字符串字面量或函数定义中触发（[clangd#51](https://github.com/clangd/clangd/issues/51)、[clangd#289](https://github.com/clangd/clangd/issues/289)）

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

| 能力                           | 状态 | 问题 |
| ------------------------------ | ---- | ---- |
| 函数重载                       | 支持 |      |
| 当前形参跟踪                   | 支持 |      |
| 成员函数重载                   | 支持 |      |
| 标签中的默认实参               | 支持 |      |
| C 风格可变参数函数             | 支持 |      |
| 可变参数模板参数包             | 支持 |      |
| 当前形参超出较短重载的参数范围 | 支持 |      |

### 函数重载

被调用函数的每个重载都带有参数列表和返回类型

```cpp
void foo();
void foo(int x);
void foo(int x, int y);

int main() {
    foo();
}
```

### 当前形参跟踪

光标所在的形参会用方括号括起；光标位于第二个实参中

```cpp
void bar(int first, double second, char third);

int main() {
    bar(1, 2.0, 'c');
}
```

### 成员函数重载

非 const 接收对象会列出 const 与非 const 重载；标签中不显示末尾的 const 限定符

```cpp
struct Buffer {
    int at(int index);
    int at(int index) const;
};

int main() {
    Buffer b;
    b.at(0);
}
```

### 标签中的默认实参

带默认值的形参会在签名中显示其初始化式

```cpp
void configure(int width, int height = 100, bool visible = true);

int main() {
    configure(1);
}
```

### C 风格可变参数函数

列出具名形参，标签中省略末尾的省略号

```cpp
void record(int code, ...);

int main() {
    record(0);
}
```

### 可变参数模板参数包

参数包显示为被调用函数未实例化时的签名

```cpp
template <typename... Args>
void emit(Args... args);

int main() {
    emit();
}
```

### 当前形参超出较短重载的参数范围

光标位于第二个实参中时，仅保留声明了第二个形参的重载

```cpp
void draw();
void draw(int x);
void draw(int x, int y);

int main() {
    draw(1, 2);
}
```

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

- [ ] 按参数个数过滤依赖型重载候选项（[clangd#2342](https://github.com/clangd/clangd/issues/2342)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo(1, 2);  // if T has foo(int) and foo(int,int), only show foo(int,int) as viable
  }
  ```

- [ ] 改进依赖型重载的启发式解析（[clangd#1083](https://github.com/clangd/clangd/issues/1083)）

- [ ] 从显示的签名中去除 C++23 显式对象形参（[clangd#2284](https://github.com/clangd/clangd/issues/2284)）

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show "(int x)", not "(this S& self, int x)"
  ```

## 特殊调用上下文

<!-- BEGIN GENERATED ITEMS: special_call_contexts -->

| 能力             | 状态 | 问题                                                                                                                   |
| ---------------- | ---- | ---------------------------------------------------------------------------------------------------------------------- |
| 构造函数和聚合体 | 支持 | [clangd#726](https://github.com/clangd/clangd/issues/726)、[clangd#2541](https://github.com/clangd/clangd/issues/2541) |
| 函数指针调用     | 支持 |                                                                                                                        |
| 模板实参列表     | 支持 | [clangd#299](https://github.com/clangd/clangd/issues/299)、[clangd#1387](https://github.com/clangd/clangd/issues/1387) |
| 嵌套调用         | 支持 |                                                                                                                        |
| 仿函数调用       | 支持 |                                                                                                                        |
| Lambda 调用      | 支持 |                                                                                                                        |
| new 表达式       | 支持 |                                                                                                                        |

### 构造函数和聚合体

构造函数调用不显示返回箭头；聚合初始化在花括号中列出字段

```cpp
struct Point {
    int x;
    int y;
};

struct Widget {
    Widget(int a, double b);
};

int main() {
    Point p{1, 2};
    Widget w(3, 4.0);
}
```

### 函数指针调用

显示原型的参数名，而不仅仅是类型

```cpp
int main() {
    void (*callback)(int code, double value) = nullptr;
    callback(5, 1.5);
}
```

### 模板实参列表

模板参数显示为签名；类模板会标明其类别，而非返回类型

```cpp
template <typename T, typename U>
struct Pair {};

Pair<int,  double> p;
```

### 嵌套调用

内层调用的签名帮助显示在内层标记处，外层调用的签名帮助显示在外层标记处

```cpp
int inner(int a);
int outer(int b, int c);

int main() {
    outer(inner(1), 2);
}
```

### 仿函数调用

调用对象时，会针对其 operator() 重载显示签名帮助

```cpp
struct Adder {
    int operator()(int a, int b);
};

int main() {
    Adder add;
    add(1, 2);
}
```

### Lambda 调用

调用 Lambda 变量时会显示闭包的 operator() 参数

```cpp
int main() {
    auto square = [](int n) {
        return n * n;
    };
    square(3);
}
```

### new 表达式

new 表达式的构造函数参数用于提供签名帮助

```cpp
struct Node {
    Node(int value, Node* next);
};

int main() {
    Node* n = new Node(0, nullptr);
}
```

<!-- END GENERATED ITEMS -->

- [ ] 继承构造函数 — 从派生类调用时显示基类构造函数（[clangd#1363](https://github.com/clangd/clangd/issues/1363)）

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

- [ ] Lambda 调用 — 显示 Lambda 名称而非 `operator()`（[clangd#86](https://github.com/clangd/clangd/issues/86)）

  ```cpp
  auto validate = [](int x, int max) -> bool { ... };
  validate(^  // show "validate(int x, int max) -> bool", not "operator()(int x, int max)"
  ```

- [ ] 函数指针调用 — 显示参数名（[clangd#1068](https://github.com/clangd/clangd/issues/1068)、[clangd#1729](https://github.com/clangd/clangd/issues/1729)）

  ```cpp
  void (*callback)(int status, const char* msg);
  callback(^  // show "(int status, const char* msg)"
  ```

- [ ] 对象初始化时的构造函数签名帮助

- [ ] 宏函数调用 — 显示宏参数而非底层展开（[clangd#795](https://github.com/clangd/clangd/issues/795)）

  ```cpp
  #define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)
  CHECK(^  // show "CHECK(cond, msg)", not "fail(const char*)"
  ```

## 参数显示

- [ ] 转发函数参数解析 — 对 `std::make_unique`、`emplace_back` 等显示底层构造函数参数（[clangd#517](https://github.com/clangd/clangd/issues/517)）

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
- [ ] 通过继承构造函数传播文档（[clangd#1936](https://github.com/clangd/clangd/issues/1936)）
- [ ] 重载集数量指示器
