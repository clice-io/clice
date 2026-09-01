# 签名帮助

## 触发字符

已注册：`(`、`)`、`{`、`}`、`<`、`>`、`,`

| 字符 | 上下文       | 行为         |
| ---- | ------------ | ------------ |
| `(`  | 函数调用     | 显示重载签名 |
| `)`  | 右括号       | 更新上下文   |
| `{`  | 花括号初始化 | 显示重载签名 |
| `}`  | 右花括号     | 更新上下文   |
| `<`  | 模板参数     | 显示重载签名 |
| `>`  | 模板关闭     | 更新上下文   |
| `,`  | 参数分隔符   | 更新活跃参数 |

- [ ] 避免误触发 — 不在注释、字符串字面量或函数定义中触发（[clangd#51](https://github.com/clangd/clangd/issues/51)、[clangd#289](https://github.com/clangd/clangd/issues/289)）

  ```cpp
  void foo(int x, int y) {  // should NOT trigger signature help
  //       ^^^^^^^^^^^^^ this is a definition, not a call
  ```

- [ ] `new` 表达式的花括号应触发签名帮助（[clangd#1967](https://github.com/clangd/clangd/issues/1967)）

  ```cpp
  auto* w = new Widget{800, 600};
  //                   ^ should trigger signature help for Widget constructors
  ```

## 重载签名

<!-- BEGIN GENERATED ITEMS: Overload Signatures -->

- [x] 函数重载 — 被调用者的每个重载，各带参数列表和返回类型

  <details>
  <summary>示例</summary>

  ```cpp
  void foo();
  void foo(int x);
  void foo(int x, int y);

  int main() {
      foo();
  }
  ```

  </details>

- [x] 活跃参数追踪 — 光标下的参数被括起；光标位于第二个参数

  <details>
  <summary>示例</summary>

  ```cpp
  void bar(int first, double second, char third);

  int main() {
      bar(1, 2.0, 'c');
  }
  ```

  </details>

- [x] 成员函数重载 — 非 const 接收者列出 const 与非 const 重载；尾随 const 限定符不在标签中显示

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 标签中的默认参数 — 带默认值的参数在签名中显示其初始化式

  <details>
  <summary>示例</summary>

  ```cpp
  void configure(int width, int height = 100, bool visible = true);

  int main() {
      configure(1);
  }
  ```

  </details>

- [x] C 风格可变参数函数 — 列出命名参数，标签中省略尾部的省略号

  <details>
  <summary>示例</summary>

  ```cpp
  void record(int code, ...);

  int main() {
      record(0);
  }
  ```

  </details>

- [x] 可变参数模板包 — 参数包渲染为被调用方的未实例化签名

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename... Args>
  void emit(Args... args);

  int main() {
      emit();
  }
  ```

  </details>

- [x] 活跃参数越过较短重载 — 光标位于第二个参数时，仅保留声明了第二个参数的重载

  <details>
  <summary>示例</summary>

  ```cpp
  void draw();
  void draw(int x);
  void draw(int x, int y);

  int main() {
      draw(1, 2);
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] 模板实例化模式解析（显示模板模式，而非实例化）
- [ ] 过滤 const/non-const 重载副本 — 当只有一个可行时不要同时显示两者（[clangd#50](https://github.com/clangd/clangd/issues/50)）

  ```cpp
  struct Vec {
      int& operator[](size_t);
      const int& operator[](size_t) const;
  };
  Vec v;
  v[0];  // only show non-const overload (v is non-const)
  ```

- [ ] 优先显示用户提供的构造函数而非编译器生成的构造函数（[clangd#1259](https://github.com/clangd/clangd/issues/1259)）

- [ ] 按参数数量过滤依赖重载候选（[clangd#2342](https://github.com/clangd/clangd/issues/2342)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo(1, 2);  // if T has foo(int) and foo(int,int), only show foo(int,int) as viable
  }
  ```

- [ ] 更好的依赖重载启发式解析（[clangd#1083](https://github.com/clangd/clangd/issues/1083)）

- [ ] 从显示的签名中去除 C++23 显式对象参数（[clangd#2284](https://github.com/clangd/clangd/issues/2284)）

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show "(int x)", not "(this S& self, int x)"
  ```

## 特殊调用上下文

<!-- BEGIN GENERATED ITEMS: Special Call Contexts -->

- [x] 构造函数和聚合体 — 构造函数调用不显示返回箭头；聚合初始化在花括号中列出字段（[clangd#726](https://github.com/clangd/clangd/issues/726)、[clangd#2541](https://github.com/clangd/clangd/issues/2541)）

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 函数指针调用 — 显示原型的参数名，而不仅仅是类型

  <details>
  <summary>示例</summary>

  ```cpp
  int main() {
      void (*callback)(int code, double value) = nullptr;
      callback(5, 1.5);
  }
  ```

  </details>

- [x] 模板参数列表 — 模板参数显示为签名；类模板指向其种类，而非返回类型（[clangd#299](https://github.com/clangd/clangd/issues/299)、[clangd#1387](https://github.com/clangd/clangd/issues/1387)）

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T, typename U>
  struct Pair {};

  Pair<int,  double> p;
  ```

  </details>

- [x] 嵌套调用 — 内层调用的帮助显示在内层标记处，外层调用的帮助显示在外层标记处

  <details>
  <summary>示例</summary>

  ```cpp
  int inner(int a);
  int outer(int b, int c);

  int main() {
      outer(inner(1), 2);
  }
  ```

  </details>

- [x] 仿函数调用 — 调用对象时将签名帮助路由到其 operator() 重载

  <details>
  <summary>示例</summary>

  ```cpp
  struct Adder {
      int operator()(int a, int b);
  };

  int main() {
      Adder add;
      add(1, 2);
  }
  ```

  </details>

- [x] Lambda 调用 — 调用 lambda 变量时提供闭包的 operator() 参数

  <details>
  <summary>示例</summary>

  ```cpp
  int main() {
      auto square = [](int n) {
          return n * n;
      };
      square(3);
  }
  ```

  </details>

- [x] new 表达式 — new 表达式的构造函数参数驱动签名帮助

  <details>
  <summary>示例</summary>

  ```cpp
  struct Node {
      Node(int value, Node* next);
  };

  int main() {
      Node* n = new Node(0, nullptr);
  }
  ```

  </details>

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

- [ ] Lambda 调用 — 显示 lambda 名称而非 `operator()`（[clangd#86](https://github.com/clangd/clangd/issues/86)）

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

- [ ] 活跃参数的文档（来自 `@param` 注释）

  ```cpp
  /// @param path The file system path.
  /// @param mode POSIX file permission bits.
  void open(std::string path, int mode);
  open("file", ^  // show documentation for mode parameter
  ```

- [ ] 尊重 `documentationFormat` 能力（[clangd#945](https://github.com/clangd/clangd/issues/945)）
- [ ] 继承构造函数传播文档（[clangd#1936](https://github.com/clangd/clangd/issues/1936)）
- [ ] 重载集数量指示
