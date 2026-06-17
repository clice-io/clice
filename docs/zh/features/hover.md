# 悬停

## 符号信息

- [x] 显示符号所属的作用域上下文（命名空间、类）
- [x] 符号种类分类
- [x] 访问修饰符（public / protected / private）
- [x] 文档注释（Doxygen）
- [x] 源码定义渲染
- [x] 截断大型初始化列表的定义显示（[clangd#710](https://github.com/clangd/clangd/issues/710)）

  ```cpp
  const int table[] = {0, 1, 2, /* ... 1000 个元素 ... */};
  // 悬停 → 显示截断的定义，而非完整初始化列表
  ```

- [ ] 在悬停中显示 `virtual` / `override` / `final` 修饰符（[clangd#2474](https://github.com/clangd/clangd/issues/2474)）

  ```cpp
  struct Base {
      virtual void draw() = 0;
  };
  struct Circle : Base {
      void draw() override;  // 悬停应显示 "virtual void draw() override"
  };
  ```

- [ ] 在作用域中显示匿名命名空间（[clangd#436](https://github.com/clangd/clangd/issues/436)）

  ```cpp
  namespace { void helper(); }
  // 悬停 helper → 作用域："(anonymous namespace)::"
  ```

## 类型信息

- [x] 变量类型美化打印
- [x] 脱糖类型（`aka` 字段）
- [x] 函数/lambda 返回类型
- [x] 函数参数（类型、名称、默认值）
- [x] 模板参数
- [x] `auto` 和 `decltype` 关键字悬停显示推导类型
- [ ] 为 CTAD 变量显示推导的模板参数（[clangd#435](https://github.com/clangd/clangd/issues/435)）

  ```cpp
  std::pair p(1, 3.14);
  // 悬停 p → "std::pair<int, double> p"
  ```

- [ ] 为实例化显示模板参数（[clangd#230](https://github.com/clangd/clangd/issues/230)）

  ```cpp
  template<typename T> T identity(T x) { return x; }
  identity(42);
  // 悬停 → 模板参数中显示 "T = int"
  ```

- [ ] 从调用上下文解析 lambda `auto` 参数类型（[clangd#493](https://github.com/clangd/clangd/issues/493)）

  ```cpp
  auto cmp = [](auto a, auto b) { return a < b; };
  std::sort(v.begin(), v.end(), cmp);
  // 悬停 a → "int"（从 std::sort<int> 推导）
  ```

- [ ] 为 `auto` 变量保留糖化类型（[clangd#709](https://github.com/clangd/clangd/issues/709)）

  ```cpp
  auto it = vec.begin();
  // 当前：  "auto it" → "__gnu_cxx::__normal_iterator<int*, ...>"
  // 期望："auto it" → "std::vector<int>::iterator"
  ```

- [ ] 按 `.clang-format` 样式打印类型名（[clangd#2156](https://github.com/clangd/clangd/issues/2156)）

  ```cpp
  // .clang-format: QualifierAlignment: Right
  const int* p;
  // 当前：  悬停 → "const int *p"
  // 期望：悬停 → "int const *p"
  ```

- [ ] 修正 C typedef 匿名结构体的悬停显示（[clangd#2219](https://github.com/clangd/clangd/issues/2219)）

  ```cpp
  typedef struct { int x, y; } Point;
  Point p;
  // 当前：  悬停 → "struct Point p"（误导——不存在 struct Point）
  // 期望：悬停 → "Point p"（匿名结构体的别名）
  ```

- [ ] 概念和受约束 `auto` 的悬停显示约束信息

## 布局信息

- [x] 字段和类型的大小（位）
- [x] 在所属类中的偏移量
- [x] 填充检测
- [x] 对齐
- [ ] 为类型级悬停显示对齐和填充信息，而不仅是字段（[clangd#1763](https://github.com/clangd/clangd/issues/1763)）

  ```cpp
  struct Widget {
      int id;       // 偏移 0，大小 32
      double value; // 偏移 64，大小 64，填充 32
  };
  // 悬停 Widget → 大小：128 位，对齐：64 位，总填充：32 位
  ```

- [ ] 类方法的虚函数表偏移量（[clangd#1771](https://github.com/clangd/clangd/issues/1771)）

  ```cpp
  struct Base {
      virtual void foo();  // 悬停 → vtable 偏移：0
      virtual void bar();  // 悬停 → vtable 偏移：1
  };
  ```

## 表达式上下文

- [x] 求值的常量值（`value` 字段）
- [x] 被调用参数信息（此实参映射到哪个形参）
- [x] 传递语义（引用、const 引用、值）
- [x] 隐式转换检测
- [ ] 悬停显示字符串字面量长度（[clangd#1016](https://github.com/clangd/clangd/issues/1016)）

  ```cpp
  const char* msg = "hello world";
  //                 ^^^^^^^^^^^^^ 悬停 → 长度：11（不含 null 终止符）
  ```

- [ ] 悬停显示数值字面量类型（[clangd#1669](https://github.com/clangd/clangd/issues/1669)）

  ```cpp
  auto x = 42ULL;
  //       ^^^^^ 悬停 → 类型：unsigned long long，值：42
  auto y = 0x1p10;
  //       ^^^^^^ 悬停 → 类型：double，值：1024.0
  ```

- [ ] 避免悬停记录类型变量时显示误导性表达式值（[clangd#1622](https://github.com/clangd/clangd/issues/1622)）

  ```cpp
  constexpr int N = 10;
  std::array<int, N> arr;
  arr[0];
  // 悬停 arr → 不应显示 "value: 10"（来自外层表达式）
  ```

## 文档

- [ ] 解析 `@copydoc` Doxygen 标签（[clangd#1320](https://github.com/clangd/clangd/issues/1320)）

  ```cpp
  /// 详细文档。
  void base_func();

  /// @copydoc base_func()
  void wrapper();
  // 悬停 wrapper → 显示 base_func 的文档
  ```

- [ ] 从基类被重写方法继承文档（[clangd#2504](https://github.com/clangd/clangd/issues/2504)）

  ```cpp
  struct Base {
      /// 渲染部件。
      virtual void draw();
  };
  struct Circle : Base {
      void draw() override;
      // 悬停 → "渲染部件。"（从 Base::draw 继承）
  };
  ```

- [ ] 在连续声明的重载之间共享文档（[clangd#2506](https://github.com/clangd/clangd/issues/2506)）

  ```cpp
  /// 打开文件。
  void open(const char* path);
  void open(const char* path, int flags);
  // 悬停第二个重载 → "打开文件。"（从第一个共享）
  ```

- [ ] 继承构造函数的文档传播（[clangd#1936](https://github.com/clangd/clangd/issues/1936)）

- [ ] 仅关联紧邻声明上方的注释（[clangd#974](https://github.com/clangd/clangd/issues/974)）

  ```cpp
  // ==== 区域标题 ====

  void foo();
  // 悬停 foo → 不应显示 "==== 区域标题 ===="
  ```

- [ ] 提供抑制误关联文档注释的选项（[clangd#2148](https://github.com/clangd/clangd/issues/2148)）

- [ ] 优先使用声明处文档而非定义处注释（[clangd#829](https://github.com/clangd/clangd/issues/829)）

  ```cpp
  // header.h:
  /// 公共 API 文档。
  void process(int x);

  // source.cpp:
  // 内部实现备注
  void process(int x) { ... }
  // 悬停 → 显示 "公共 API 文档。"，而非 "内部实现备注"
  ```

- [ ] 保留注释中的空白和换行（[clangd#2057](https://github.com/clangd/clangd/issues/2057)）

  ```cpp
  /// | 列 A | 列 B |
  /// |------|------|
  /// | 1    | 2    |
  void table_fn();
  // 悬停 → 正确渲染 markdown 表格，而非合为一行
  ```

- [ ] 修复悬停注释渲染中的错误缩进（[clangd#1040](https://github.com/clangd/clangd/issues/1040)）

- [ ] 模板关键字来自宏展开时缺失文档字符串（[clangd#1226](https://github.com/clangd/clangd/issues/1226)）

- [x] 为简单访问器（getter/setter）合成文档

## 宏悬停

- [ ] 显示宏展开结果（在定义之前）（[clangd#2642](https://github.com/clangd/clangd/issues/2642)）

  ```cpp
  #define MAX(a, b) ((a) > (b) ? (a) : (b))
  int z = MAX(x, y);
  //      ^^^^^^^^^ 悬停 → 展开："((x) > (y) ? (x) : (y))"
  //                         定义："#define MAX(a, b) ((a) > (b) ? (a) : (b))"
  ```

## 特殊悬停目标

- [ ] 类型级悬停显示结构体/枚举成员（[clangd#959](https://github.com/clangd/clangd/issues/959)）

  ```cpp
  enum Color { Red, Green, Blue };
  // 悬停 Color → "enum Color { Red, Green, Blue }"
  ```

- [ ] 为 typedef 显示底层结构体定义（[clangd#2020](https://github.com/clangd/clangd/issues/2020)）

  ```cpp
  typedef struct { int x, y; } Point;
  Point p;
  // 悬停 Point → 显示结构体定义（包含成员）
  ```

- [ ] 关键字悬停文档（[clangd#1862](https://github.com/clangd/clangd/issues/1862)）

  ```cpp
  const int x = 42;
  // 悬停 "const" → 显示该关键字的说明
  ```

- [x] 属性悬停文档（[clangd#1862](https://github.com/clangd/clangd/issues/1862)）

  ```cpp
  [[nodiscard]] int compute();
  // 悬停 "nodiscard" → 显示该属性的说明
  ```

- [ ] `#include` 指令悬停显示解析后的头文件路径
- [x] `this` 表达式悬停显示指向的类型
- [x] `__func__` 及相关预定义标识符悬停

- [ ] GTK-Doc、kernel-doc 和 GObject Introspection 文档支持（[clangd#2662](https://github.com/clangd/clangd/issues/2662)）

- [ ] Doxygen `@f$` 数学公式的 LaTeX 渲染（[clangd#2669](https://github.com/clangd/clangd/issues/2669)）

## 模块相关

- [ ] 悬停 `import` 语句显示模块信息
- [ ] 悬停模块名称显示所属文件/分区列表

## 悬停正确性

应正确处理的已知问题：

- [ ] MSVC 目标下 `MSInheritanceAttr` 导致悬停错误（[clangd#1643](https://github.com/clangd/clangd/issues/1643)、[clangd#2212](https://github.com/clangd/clangd/issues/2212)）
- [ ] 对象实例化被误识别为函数声明（[clangd#2225](https://github.com/clangd/clangd/issues/2225)）
- [ ] 大无符号枚举常量值导致崩溃（[clangd#2381](https://github.com/clangd/clangd/issues/2381)）
- [ ] 带默认参数的函数调用悬停崩溃（[clangd#551](https://github.com/clangd/clangd/issues/551)）
- [ ] 与函数式宏同名的符号被错误识别（[clangd#2490](https://github.com/clangd/clangd/issues/2490)）

## 变更记录

| 日期    | 变更                 | PR     |
| ------- | -------------------- | ------ |
| 2025-06 | 移植 clangd 悬停实现 | [#452] |

[#452]: https://github.com/clice-io/clice/pull/452
