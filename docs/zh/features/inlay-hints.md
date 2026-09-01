# Inlay Hints

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/inlay_hint/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 为代码中隐式省略的信息渲染内联标注：调用处的参数名、推导出的类型，以及位置式聚合初始化背后的字段名。提示类别可以通过 `[inlay_hints]` 配置节单独开关；下面各节介绍默认开启的类别。

## 参数名提示

<!-- BEGIN GENERATED ITEMS: Parameter Hints -->

- [x] 参数名提示 — 调用处和构造函数调用中的实参名

  <details>
  <summary>示例</summary>

  ```cpp
  void draw(int width, int height);

  struct Point {
      Point(int x, int y);
      Point(const Point& other);
      Point(Point&& other);
  };

  void use() {
      draw(10, 20);
      Point p(1, 2);
      Point q{3, 4};
      // Copy and move constructors stay quiet; a temporary's own braces
      // still hint (the outer prvalue construction is elided anyway).
      Point r(p);
      Point m(Point{5, 6});
      Point s(static_cast<Point&&>(r));
  }
  ```

  </details>

- [x] 提示抑制 — 实参已经拼出参数名，以及 `/*name=*/` 注释 ([clangd#1877](https://github.com/clangd/clangd/issues/1877))

  <details>
  <summary>示例</summary>

  ```cpp
  void draw(int width, int height);

  void use() {
      int width = 5;
      int h = 2;
      // `width` matches the parameter spelling: only `height:` hints.
      draw(width, h);
      // An inline comment naming the parameter serves the same purpose;
      // a comment naming something else does not.
      draw(/*width=*/1, /*height=*/2);
      draw(/*margin=*/6, 7);
  }

  struct Sizes {
      static int width;
      int height;

      void member() {
          // A bare member access spells the parameter name: suppressed.
          draw(5, height);
      }
  };

  void qualified(Sizes s) {
      // A qualified name is not a plain spelling match.
      draw(Sizes::width, 3);
      // Neither is an access through a written base object.
      draw(4, s.height);
  }
  ```

  </details>

- [x] Setter 与内建函数抑制 — `setX(x)` 以及 `std::move`/`std::forward` 的实参保持裸写

  <details>
  <summary>示例</summary>

  ```cpp
  namespace std {

  template <typename T>
  struct remove_reference {
      using type = T;
  };

  template <typename T>
  struct remove_reference<T&> {
      using type = T;
  };

  template <typename T>
  struct remove_reference<T&&> {
      using type = T;
  };

  template <typename T>
  constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

  template <typename T>
  constexpr typename remove_reference<T>::type&& move(T&& t) noexcept;

  }  // namespace std

  struct Config {
      void setWidth(int width);
      void set_height(int height);
      // The parameter carries extra information beyond the setter name, so
      // it still hints.
      void setTimeout(int timeout_millis);
  };

  void consume(int&& sink);

  // The three-argument algorithm form of std::move is a real call whose
  // parameters deserve hints; only the single-argument cast stays bare.
  namespace std {

  template <typename T>
  T* move(T* first, T* last, T* result);

  }  // namespace std

  void use(Config& config) {
      config.setWidth(3);
      config.set_height(4);
      config.setTimeout(5);
      int value = 1;
      consume(std::move(value));
      int buffer[4];
      std::move(buffer, buffer + 2, buffer + 2);
  }
  ```

  </details>

- [x] 可变引用标记 — `&` 标记通过非 const 左值引用传递的实参 ([clangd#1123](https://github.com/clangd/clangd/issues/1123))

  <details>
  <summary>示例</summary>

  ```cpp
  void mutate(int& value);
  void observe(const int& value);
  void take(int&& value);

  void use() {
      int v = 0;
      mutate(v);
      observe(v);
      take(static_cast<int&&>(v));
  }
  ```

  </details>

- [x] 转发解析 — 经包装器转发的参数包解析到目标函数的参数名 ([clangd#2324](https://github.com/clangd/clangd/issues/2324))

  <details>
  <summary>示例</summary>

  ```cpp
  namespace std {

  template <typename T>
  struct remove_reference {
      using type = T;
  };

  template <typename T>
  constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

  }  // namespace std

  void target(int first, int second);

  template <typename... Args>
  void wrap(Args&&... args) {
      target(std::forward<Args>(args)...);
  }

  // A plain pass-through works without std::forward as well.
  void sink(int a, int b, int c);

  template <typename... Ts>
  void call_with(Ts... ts) {
      sink(ts...);
  }

  // Forwarding also resolves through packs sandwiched between fixed
  // head and tail arguments.
  int accumulate(int, int b, double);

  template <typename... Args>
  int head_tail(int a, Args&&... args) {
      return accumulate(1, std::forward<Args>(args)..., 1.0);
  }

  template <typename... Args>
  int chain(Args&&... args) {
      return head_tail(std::forward<Args>(args)...);
  }

  void use() {
      wrap(1, 2);
      call_with(1, 2, 3);
      chain(32, 42);
  }
  ```

  </details>

- [x] 来自定义的名称 — 声明中未命名的参数采用定义中的名称；前导下划线会被去掉

  <details>
  <summary>示例</summary>

  ```cpp
  void resize(int, int);

  void fill(int _value, int __count);

  int scale(int good);

  void use() {
      resize(800, 600);
      fill(1, 2);
      // When both name their parameter, the declaration wins.
      scale(7);
  }

  void resize(int width, int height) {}

  int scale(int bad) {
      return bad;
  }
  ```

  </details>

- [x] 函数指针与调用运算符 — 间接调用仍会显示其参数名 ([clangd#1734](https://github.com/clangd/clangd/issues/1734), [clangd#1742](https://github.com/clangd/clangd/issues/1742))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Callback {
      void operator()(int status, int detail) const;
  };

  void (*handler)(int status, const char* message);

  void use() {
      Callback cb;
      cb(1, 2);
      cb.operator()(3, 4);
      handler(0, "ok");
      auto cmp = [](int lhs, int rhs) { return lhs < rhs; };
      cmp(1, 2);
  }
  ```

  </details>

- [x] Deducing `this` — 显式对象参数从不提示（C++23） ([clangd#1777](https://github.com/clangd/clangd/issues/1777))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget {
      void resize(this Widget& self, int width, int height);
  };

  void use() {
      Widget w;
      w.resize(800, 600);
  }
  ```

  </details>

- [x] 依赖调用 — 即使被调用方只在模板内才可知，参数名仍会出现

  候选者按实参数量匹配；只有唯一幸存的候选者会命名参数，因此仍可能命中多个重载的调用会保持裸写而不会猜测。

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  void apply(T scale);

  template <typename T>
  struct Holder {
      void member(T item);
      static void static_member(T slot);
  };

  void overload(int value);
  void overload(double value);

  template <typename T>
  struct Runner {
      void run(Holder<T> holder, T value) {
          apply(value);
          holder.member(value);
          Holder<T>::static_member(value);
          // Several overloads remain viable: no hint.
          overload(T{});
      }
  };
  ```

  </details>

- [x] 未展开的参数包 — 写出的包展开会破坏 1:1 实参映射，并停止提示

  <details>
  <summary>示例</summary>

  ```cpp
  void plot(int x, int y, int z);

  template <typename... Ts>
  void relay(Ts... ts) {
      // `ts...` may instantiate to any number of arguments.
      plot(0, ts...);
  }

  void use() {
      // The outer call still resolves through pack forwarding: 1 and 2 land
      // in plot's y and z.
      relay(1, 2);
  }
  ```

  </details>

- [x] 调用处的宏 — 拼写为宏的实参会提示；宏体内部生成的调用不提示 ([clangd#2620](https://github.com/clangd/clangd/issues/2620))

  <details>
  <summary>示例</summary>

  ```cpp
  void report(double value);
  void plot(double x, double y);
  int check(int status);

  #define PI 3.14
  #define CALL_REPORT() report(2.71)
  #define PAIR 1.0, 2.0
  #define ASSERT(expr) if(!(expr)) {}

  void use() {
      // An object-like macro is still one written argument.
      report(PI);
      // The call only exists inside the macro body.
      CALL_REPORT();
      // One macro covering several arguments has no place to anchor.
      plot(PAIR);
      // Code written as a macro argument keeps its hints.
      ASSERT(check(42) == 0);
  }
  ```

  </details>

- [x] 隐式构造函数调用 — 代码从未写出的转换不会产生它们自己的提示

  <details>
  <summary>示例</summary>

  ```cpp
  struct Seconds {
      Seconds(int raw);
  };

  void wait(Seconds);
  void hold(Seconds duration);

  Seconds use() {
      // The implicit Seconds(5) must not surface `raw:`.
      wait(5);
      // The written call still hints its own parameter.
      hold(6);
      // Nor does the conversion in a return statement.
      return 7;
  }
  ```

  </details>

- [x] 伪对象表达式 — MS 属性访问保持安静；写出的下标保留访问器的名称

  <details>
  <summary>示例</summary>

  ```cpp
  int printf(const char* Format, ...);

  struct State {
      __declspec(property(get = GetX, put = PutX)) int x[];
      int GetX(int row, int column);
      void PutX(int value);

      // The syntactic form is a binary operator: no `value:` hint on `y`.
      void Work(int y) {
          x = y;
      }
  };

  int use() {
      State s;
      // The semantic form of __builtin_dump_struct calls printf; none of it
      // is written here.
      __builtin_dump_struct(&s, printf);
      printf("%d", 42);
      // Property subscripts read best with the accessor's parameter names.
      return s.x[1][2];
  }
  ```

  </details>

- [x] 显式实例化 — 显式实例化定义不会增加重复提示，而其写出的模板实参正常提示 ([clangd#1034](https://github.com/clangd/clangd/issues/1034))

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  void apply(T value) {}

  template void apply<int>(int value);

  void use() {
      apply(42);
  }

  int measure(int amount);

  template <typename T>
  struct Box {};

  template struct Box<decltype(measure(7))>;
  ```

  </details>

- [ ] 宽松名称匹配 — `aParam` 尚不能抑制拼写为 `param` 的实参 _(部分)_ ([clangd#2248](https://github.com/clangd/clangd/issues/2248))

  <details>
  <summary>示例</summary>

  ```cpp
  void draw(int aParam);

  void use() {
      int param = 3;
      // Ideally the near-match would suppress the hint; today it still shows.
      draw(param);
  }
  ```

  </details>

- [ ] 继承构造函数 — `using Base::Base` 调用会丢失参数名 _(部分)_ ([clangd#1364](https://github.com/clangd/clangd/issues/1364))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Base {
      Base(int width);
  };

  struct Derived : Base {
      using Base::Base;
  };

  // No `width:` hint yet.
  Derived d(7);
  ```

  </details>

- [x] 匿名参数 — 没有名称可提示，不过可变引用仍会标记 `&`

  <details>
  <summary>示例</summary>

  ```cpp
  void value_sink(int);
  void ref_sink(int&);
  void const_ref_sink(const int&);
  void rvalue_sink(int&&);

  void use() {
      int v = 0;
      value_sink(1);
      // Only the `&` marker survives without a name.
      ref_sink(v);
      const_ref_sink(v);
      rvalue_sink(2);
  }
  ```

  </details>

- [x] 运算符与字面量 — 运算符语法和用户定义字面量保持裸写；成员初始化器和默认成员初始化器会提示

  <details>
  <summary>示例</summary>

  ```cpp
  struct S {
      S(int param);
  };

  void operator+(S lhs, S rhs);

  long double operator""_w(long double param);

  struct Holder {
      S member;
      S defaulted{3};
      Holder() : member(42) {}
  };

  void use() {
      S a(1);
      S b(2);
      a + b;
      1.2_w;
  }
  ```

  </details>

- [ ] 构造函数实参中的参数包 — 外层调用可解析；展开内部的提示仍未实现 _(部分)_

  <details>
  <summary>示例</summary>

  ```cpp
  struct Foo {
      Foo();
      Foo(int x);
  };

  void consume(Foo a, int b);

  template <typename... Args>
  void relay(Args... args) {
      consume(args...);
  }

  template <typename... Args>
  void construct(Args... args) {
      // The written Foo{args...} and the literal after it get no hints yet.
      consume(Foo{args...}, 1);
  }

  void use() {
      relay(Foo{}, 42);
      relay(42, 42);
      construct(42);
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 类型提示

<!-- BEGIN GENERATED ITEMS: Type Hints -->

- [x] `auto` 推导变量 — 提示显示完整变量类型，包括限定符

  <details>
  <summary>示例</summary>

  ```cpp
  int make();

  void use() {
      auto value = make();
      const auto& ref = value;
      auto* ptr = &value;
  }
  ```

  </details>

- [x] 类型糖与长度限制 — 别名保留其拼写；超长类型回退到糖化名称（[clangd#1298](https://github.com/clangd/clangd/issues/1298)、[clangd#1357](https://github.com/clangd/clangd/issues/1357)）

  <details>
  <summary>示例</summary>

  ```cpp
  using Integer = int;

  Integer make_alias();

  template <typename A, typename B, typename C>
  struct extremely_long_template_name {};

  using Compact = extremely_long_template_name<int, char, bool>;

  Compact make_compact();

  extremely_long_template_name<Integer, Integer, Integer> make_long();

  template <typename T, typename U = int>
  struct Defaulted {};

  Defaulted<float> make_defaulted();

  void use() {
      auto aliased = make_alias();
      auto shortened = make_compact();
      // No sugar short enough to fall back to: the hint is dropped.
      auto dropped = make_long();
      // Default template arguments never print.
      auto defaulted = make_defaulted();
  }
  ```

  </details>

- [x] Structured bindings — 每个绑定提示其规范类型；聚合对象本身保持无提示

  <details>
  <summary>示例</summary>

  ```cpp
  struct Pair {
      int first;
      float second;
  };

  Pair make();

  int array[2];

  void use() {
      auto [a, b] = make();
      auto [x, y] = array;
  }
  ```

  </details>

- [x] Lambda — 变量、推导返回类型和 init-capture 均提示（[clangd#1163](https://github.com/clangd/clangd/issues/1163)）

  <details>
  <summary>示例</summary>

  ```cpp
  int compute();

  void use() {
      auto callback = [captured = compute()](int x) {
          return x + captured;
      };
      auto bare = [] {
          return 1.5;
      };
  }
  ```

  </details>

- [x] 推导返回类型 — 参数列表后的 `-> T`，声明也包含在内

  <details>
  <summary>示例</summary>

  ```cpp
  auto answer() {
      return 42;
  }

  auto& ref_answer() {
      static int storage = 0;
      return storage;
  }

  // A declaration hints once a later definition supplies the deduction; a
  // definition-less one stays silent.
  auto declared(int x);
  auto deducible(int x);

  auto deducible(int x) {
      return x + 1;
  }

  // Written trailing return types need no hint.
  auto spelled() -> int;
  auto pointer() -> auto* {
      return "text";
  }

  struct Convertible {
      operator auto() {
          return 42;
      }
  };
  ```

  </details>

- [x] `decltype` 拼写 — 底层类型显示在写出的 `decltype` 旁边

  <details>
  <summary>示例</summary>

  ```cpp
  int source();

  decltype(source()) value = 1;

  int& ref = value;
  // decltype(auto) preserves the reference.
  decltype(auto) forwarded = ref;

  // Every written decltype spelling hints: declarators, alias targets,
  // return types and functional casts.
  const decltype(0)& bound = value;

  decltype(0) declared();

  auto trailing() -> decltype(0);

  template <class, class>
  struct Wrap;

  using Alias = Wrap<decltype(0), float>;

  auto constructed = decltype(0){};
  ```

  </details>

- [x] `auto` 参数 — 恰好一个实例化的模板会显示推导出的类型

  <details>
  <summary>示例</summary>

  ```cpp
  int twice(auto x) {
      return x + x;
  }

  int result = twice(21);

  // A second instantiation makes the deduction ambiguous: no hint.
  int measure(auto x) {
      return 1;
  }

  int a = measure(1);
  int b = measure(2.0);

  // Packs and parameters after them never hint.
  int spread(auto first, auto... rest, auto last) {
      return 0;
  }

  int c = spread<void*, char, float>(nullptr, 'x', 2.0f, 3);

  // Deduplication: a template body hints once across instantiations of the
  // same deduced type.
  template <typename T>
  void body() {
      auto var = 42;
  }

  template void body<int>();
  template void body<float>();
  ```

  </details>

- [ ] 显式拼写的初始化器 — 强制转换和函数式转换仍会冗余提示*（部分）*（[clangd#1749](https://github.com/clangd/clangd/issues/1749)）

  <details>
  <summary>示例</summary>

  ```cpp
  int compute();

  void use() {
      // The type is already written on the right-hand side; ideally these
      // two hints would be suppressed.
      auto widened = static_cast<long>(compute());
      auto braced = int{42};
  }
  ```

  </details>

- [ ] 依赖 `auto` — 未实例化的模板体内部推导保持静默*（部分）*（[clangd#2275](https://github.com/clangd/clangd/issues/2275)）

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  void body(T input) {
      // No hint: the deduced type depends on T.
      auto derived = input + 1;
      // A dependence-free initializer still hints normally.
      auto counter = 0;
  }
  ```

  </details>

- [x] 作用域抑制 — 命名空间限定从提示中去掉；类作用域保留

  <details>
  <summary>示例</summary>

  ```cpp
  namespace outer {
  namespace inner {

  struct S1 {};
  S1 make_s1();
  auto x = make_s1();

  struct S2 {
      template <typename T>
      struct Nested {};
  };

  S2::Nested<int> make_nested();
  auto y = make_nested();

  }  // namespace inner
  }  // namespace outer
  ```

  </details>

- [x] Tuple 协议绑定 — 提示打印规范类型，而不是 `tuple_element<I, T>::type`

  <details>
  <summary>示例</summary>

  ```cpp
  struct IntPair {
      int a;
      int b;
  };

  namespace std {

  template <typename T>
  struct tuple_size {};

  template <>
  struct tuple_size<IntPair> {
      constexpr static unsigned value = 2;
  };

  template <unsigned I, typename T>
  struct tuple_element {};

  template <unsigned I>
  struct tuple_element<I, IntPair> {
      using type = int;
  };

  }  // namespace std

  template <unsigned I>
  int get(const IntPair& p) {
      if constexpr(I == 0) {
          return p.a;
      } else {
          return p.b;
      }
  }

  IntPair make();

  auto [x, y] = make();
  ```

  </details>

- [ ] 实例化模板 — 实例化后的函数体不在模式处重复提示；当恰好存在一个实例化时，依赖 `auto` 可以显示推导类型*（部分）*（[clangd#2275](https://github.com/clangd/clangd/issues/2275)）

  <details>
  <summary>示例</summary>

  ```cpp
  void take(int first, int second);

  template <typename T>
  struct Single {
      void reset() {
          take(1, 2);
          // Deducible from the only instantiation, but not yet deduced.
          auto copy = T();
      }
  };

  template struct Single<char>;

  template <typename T>
  struct Twice {
      void reset() {
          // No hint: two instantiations deduce contradicting types.
          auto copy = T();
      }
  };

  template struct Twice<char>;
  template struct Twice<int>;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Designator 提示

<!-- BEGIN GENERATED ITEMS: Designator Hints -->

- [x] 字段和索引 designator — 位置式聚合初始化显示 `.field=` 和 `[index]=`（[clangd#2303](https://github.com/clangd/clangd/issues/2303)）

  <details>
  <summary>示例</summary>

  ```cpp
  struct Point {
      int x;
      int y;
      int z;
  };

  Point p{1, 2 + 2};

  int coordinates[2] = {7, 8};

  // Array designators survive dependent-sized members; reserved names are
  // skipped rather than printed.
  template <typename T, int N>
  struct Array {
      T __elements[N];
  };

  Array<int, 2> pair = {0, 1};
  ```

  </details>

- [x] 嵌套聚合 — 写出的花括号递归；省略的花括号扁平化为 `.outer.inner=`

  <details>
  <summary>示例</summary>

  ```cpp
  struct Inner {
      int x;
      int y;
  };

  struct Outer {
      Inner a;
      Inner b;
  };

  Outer o{{1, 2}, 3};
  ```

  </details>

- [x] 匿名成员 — 未命名的 union 和 struct 从 designator 路径中消失

  <details>
  <summary>示例</summary>

  ```cpp
  struct State {
      union {
          struct {
              struct {
                  int y;
              };
          } x;
      };
  };

  State s{42};
  ```

  </details>

- [x] designator 抑制 — 已写出的 designator 和 `/*name=*/` 注释保持其初始化值无提示

  <details>
  <summary>示例</summary>

  ```cpp
  struct Point {
      int a;
      int b;
      int c;
      int d;
      int e;
  };

  // Mixing written designators with positional inits is a C99 extension
  // clang accepts with a warning; only the bare `4` needs help.
  Point p{/*a=*/1, .c = 2, /* .d = */ 3, 4};
  ```

  </details>

- [x] 仅聚合 — 构造函数调用、复制和惯用的零初始化不产生 designator

  <details>
  <summary>示例</summary>

  ```cpp
  struct Constructible {
      Constructible(int amount);
  };

  // A braced constructor call names parameters, not fields.
  Constructible built{5};

  struct Copyable {
      int x;
  };

  Copyable original{1};
  Copyable duplicate{original};

  // The idiomatic `{}` zero-initializer stays quiet.
  struct Wide {
      int fields[8];
  };

  Wide zeroed{};
  ```

  </details>

- [x] 损坏的初始化器 — 即便旁边初始化器编译失败，designator 仍保留

  <details>
  <summary>示例</summary>

  ```cpp
  // The first initializer deliberately fails to convert.
  struct Empty {};

  struct Mixed {
      int a;
      int b;
  };

  void use() {
      Mixed m{Empty(), 1};
  }
  ```

  </details>

- [ ] 括号聚合初始化 — C++20 `Point(1, 2)` 尚不产生提示（[clangd#2540](https://github.com/clangd/clangd/issues/2540)）

  <details>
  <summary>示例</summary>

  ```cpp
  struct Point {
      int x;
      int y;
  };

  Point p(1, 2);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 其他提示类型

<!-- BEGIN GENERATED ITEMS: Other Hint Kinds -->

- [ ] 模板参数提示 — 调用点处推导的和显式的模板实参（[clangd#2583](https://github.com/clangd/clangd/issues/2583)）

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T, typename U>
  T convert(U val);

  // Could hint `T: float` next to the explicit argument list.
  float converted = convert<float>(42);
  ```

  </details>

- [ ] CTAD 实参 — 模板名后推导出的类模板实参（[clangd#2331](https://github.com/clangd/clangd/issues/2331)）

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename A, typename B>
  struct Pair {
      A first;
      B second;
      Pair(A a, B b);
  };

  // Could hint `<int, double>` after `pair`.
  Pair pair(1, 2.5);
  ```

  </details>

- [ ] 隐式转换提示 — 显示调用点发生的转换（[clangd#2254](https://github.com/clangd/clangd/issues/2254)）

  <details>
  <summary>示例</summary>

  ```cpp
  void process(double val);

  // Could hint `(double)` before the argument.
  void use() {
      process(42);
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 块结尾提示

默认关闭（`inlay_hints.block_end`）。对于至少跨两行的代码块，在其右花括号后 clice 显示该花括号所关闭内容的名称——函数、类型、命名空间和控制流语句：

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

条件摘要会为存在简短拼写的 `if`/`while`/`switch`/`for` 打印；`else if` 链提示为普通 `// if`。超过 60 字符的标签被丢弃。

一个相关的想法，显示匹配条件的 `#endif` 提示（[clangd#2487](https://github.com/clangd/clangd/issues/2487)），尚未实现。

## 默认参数提示

默认关闭（`inlay_hints.default_arguments`）。依赖默认参数的调用点会显示被省略的内容，如果超过类型名长度限制则缩写：

```cpp
void log(int level, bool flush = true, int repeat = 1);
log(2);
//     ^ , flush: true, repeat: 1
```

## 配置

`clice.toml` 的 `[inlay_hints]` 段（或通过 `initializationOptions` 提供的同名键）控制所有类别：`enabled`、`parameters`、`deduced_types`、`designators`、`block_end`、`default_arguments` 和 `type_name_limit`。详见[配置指南](../guide/configuration.md#inlay-hints)。配置更改在服务器重启后生效——从不涉及重新编译。

## 交互行为

- 请求按范围限定：请求范围之外的提示会被丢弃。
- 参数提示锚定在实参左侧；类型提示和 designator 提示锚定在其声明侧，使用 LSP padding 标志而不是嵌入空格。
- 重复的相同提示（例如来自模板实例化的）会合并为一条。

## 其他已知缺口

- 通过 `InlayHintLabelPart` 提供可展开标签部件的缩写类型提示（[clangd#2269](https://github.com/clangd/clangd/issues/2269)）
- 可点击的类型名——在提示的类型上跳转到定义（[clangd#1535](https://github.com/clangd/clangd/issues/1535)）
- 作用域感知的类型缩短——在 `namespace foo` 内部显示 `Bar` 而不是 `foo::Bar`（[clangd#2270](https://github.com/clangd/clangd/issues/2270)）
- 协程返回模板类型时丢失参数提示（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）
