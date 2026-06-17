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
  void foo(int x, int y) {  // 不应触发签名帮助
  //       ^^^^^^^^^^^^^ 这是定义，不是调用
  ```

- [ ] `new` 表达式的花括号应触发签名帮助（[clangd#1967](https://github.com/clangd/clangd/issues/1967)）

  ```cpp
  auto* w = new Widget{800, 600};
  //                   ^ 应触发 Widget 构造函数的签名帮助
  ```

## 重载签名

- [x] 函数重载签名
- [x] 活跃参数追踪
- [x] 模板实例化模式解析（显示模板模式而非实例化）
- [x] 带类型的参数标签
- [x] 签名标签中的返回类型
- [x] 参数标签字节偏移以精确高亮
- [ ] 过滤 const/non-const 重载副本 — 仅显示可行的重载（[clangd#50](https://github.com/clangd/clangd/issues/50)）

  ```cpp
  struct Vec {
      int& operator[](size_t);
      const int& operator[](size_t) const;
  };
  Vec v;
  v[0];  // 仅显示 non-const 重载（v 是 non-const）
  ```

- [ ] 优先显示用户提供的构造函数而非编译器生成的（[clangd#1259](https://github.com/clangd/clangd/issues/1259)）

- [ ] 按参数数量过滤依赖重载候选（[clangd#2342](https://github.com/clangd/clangd/issues/2342)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo(1, 2);  // 若 T 有 foo(int) 和 foo(int,int)，仅显示 foo(int,int) 为可行
  }
  ```

- [ ] 更好的依赖重载启发式解析（[clangd#1083](https://github.com/clangd/clangd/issues/1083)）

- [ ] 去除 C++23 显式对象参数（[clangd#2284](https://github.com/clangd/clangd/issues/2284)）

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // 显示 "(int x)"，而非 "(this S& self, int x)"
  ```

## 特殊调用上下文

- [x] 模板参数签名帮助（[clangd#299](https://github.com/clangd/clangd/issues/299)、[clangd#1387](https://github.com/clangd/clangd/issues/1387)）

  ```cpp
  template<typename Key, typename Value, typename Compare = less<Key>>
  class SortedMap;

  SortedMap<int, ^  // 显示：Key = int, Value = ?, Compare = less<Key>
  ```

- [x] 聚合初始化 — 将字段名显示为"参数"（[clangd#726](https://github.com/clangd/clangd/issues/726)、[clangd#2541](https://github.com/clangd/clangd/issues/2541)）

  ```cpp
  struct Point { int x, y, z; };
  Point p = {1, ^  // 显示：.x = int, .y = int, .z = int（活跃：.y）
  ```

- [ ] 继承构造函数 — 从派生类调用时显示基类构造函数（[clangd#1363](https://github.com/clangd/clangd/issues/1363)）

  ```cpp
  struct Base { Base(int x, int y); };
  struct Derived : Base { using Base::Base; };
  Derived d(^  // 显示 Base(int x, int y)
  ```

- [ ] `operator[]` 签名帮助（[clangd#2472](https://github.com/clangd/clangd/issues/2472)）

  ```cpp
  std::map<std::string, int> m;
  m[^  // 显示 operator[](const string& key)
  ```

- [ ] Lambda 调用 — 显示 lambda 名称而非 `operator()`（[clangd#86](https://github.com/clangd/clangd/issues/86)）

  ```cpp
  auto validate = [](int x, int max) -> bool { ... };
  validate(^  // 显示 "validate(int x, int max) -> bool"，而非 "operator()(int x, int max)"
  ```

- [ ] 函数指针调用 — 显示参数名（[clangd#1068](https://github.com/clangd/clangd/issues/1068)、[clangd#1729](https://github.com/clangd/clangd/issues/1729)）

  ```cpp
  void (*callback)(int status, const char* msg);
  callback(^  // 显示 "(int status, const char* msg)"
  ```

- [ ] 对象初始化时的构造函数签名帮助

- [ ] 宏函数调用 — 显示宏参数而非底层展开（[clangd#795](https://github.com/clangd/clangd/issues/795)）

  ```cpp
  #define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)
  CHECK(^  // 显示 "CHECK(cond, msg)"，而非 "fail(const char*)"
  ```

## 参数显示

- [ ] 转发函数参数解析 — 对 `std::make_unique`、`emplace_back` 等显示底层构造函数参数（[clangd#517](https://github.com/clangd/clangd/issues/517)）

  ```cpp
  struct Widget { Widget(int width, int height); };
  std::make_unique<Widget>(^  // 显示 "(int width, int height)"
  ```

- [ ] 参数包显示（[clangd#638](https://github.com/clangd/clangd/issues/638)）

  ```cpp
  template<typename... Args>
  void log(const char* fmt, Args&&... args);
  log("x=%d y=%d", ^  // 显示 "fmt, args..." 并将活跃参数标记在 args 上
  ```

- [ ] 美化标准库参数名（[clangd#736](https://github.com/clangd/clangd/issues/736)）

  ```
  // 当前：push_back(const value_type& __x)
  // 期望：push_back(const value_type& value)
  ```

- [ ] 保留枚举类作用域（[clangd#2475](https://github.com/clangd/clangd/issues/2475)）

  ```cpp
  enum class Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // 显示 "(Color c)"，不要去除作用域
  ```

- [ ] 显示默认参数值

  ```cpp
  void open(std::string path, int mode = 0644);
  open("file", ^  // 显示 "int mode = 0644"（活跃），用户知道可以省略
  ```

## 文档

- [ ] 活跃参数的文档（来自 `@param` 注释）

  ```cpp
  /// @param path 文件系统路径。
  /// @param mode POSIX 文件权限位。
  void open(std::string path, int mode);
  open("file", ^  // 显示 mode 参数的文档
  ```

- [ ] 尊重 `documentationFormat` 能力（[clangd#945](https://github.com/clangd/clangd/issues/945)）
- [ ] 继承构造函数传播文档（[clangd#1936](https://github.com/clangd/clangd/issues/1936)）
- [ ] 重载集数量指示

## 变更记录

| 日期 | 变更                                 | PR  |
| ---- | ------------------------------------ | --- |
| —    | 函数重载签名、活跃参数追踪、触发字符 | —   |
