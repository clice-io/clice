# Inlay Hints

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/inlay_hint/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/feature_docs.ts update`. -->

clice renders inline annotations for the information the code leaves implicit: parameter names at call sites, deduced types, and the field names behind positional aggregate initialization. Hint categories can be toggled individually through the `[inlay_hints]` configuration section; the sections below describe the categories that are on by default.

## Parameter Hints

<!-- BEGIN GENERATED ITEMS: Parameter Hints -->

- [x] Parameter name hints — argument names at call sites and constructor calls

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

- [x] Hint suppression — arguments that already spell the parameter name, and `/*name=*/` comments ([clangd#1877](https://github.com/clangd/clangd/issues/1877))

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

- [x] Setter and builtin suppression — `setX(x)` and `std::move`/`std::forward` arguments stay bare

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

- [x] Mutable reference markers — `&` flags arguments passed by non-const lvalue reference ([clangd#1123](https://github.com/clangd/clangd/issues/1123))

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

- [x] Forwarding resolution — packs forwarded through wrappers resolve to the target's parameter names ([clangd#2324](https://github.com/clangd/clangd/issues/2324))

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

- [x] Names from definitions — unnamed declaration parameters take the definition's names; leading underscores strip

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

- [x] Function pointers and call operators — indirect calls still name their parameters ([clangd#1734](https://github.com/clangd/clangd/issues/1734), [clangd#1742](https://github.com/clangd/clangd/issues/1742))

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

- [x] Deducing `this` — the explicit object parameter never hints (C++23) ([clangd#1777](https://github.com/clangd/clangd/issues/1777))

  ```cpp
  struct Widget {
      void resize(this Widget& self, int width, int height);
  };

  void use() {
      Widget w;
      w.resize(800, 600);
  }
  ```

- [x] Dependent calls — parameter names appear even when the callee is only known inside a template

  Candidates are matched by argument count; only a unique surviving
  candidate names the parameters, so a call that could still hit several
  overloads stays bare rather than guessing.

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

- [x] Unexpanded packs — a written pack expansion breaks the 1:1 argument mapping and stops hinting

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

- [x] Macros at call sites — arguments spelled as macros hint; calls generated inside macro bodies do not ([clangd#2620](https://github.com/clangd/clangd/issues/2620))

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

- [x] Implicit constructor calls — conversions the code never wrote produce no hints of their own

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

- [x] Pseudo-object expressions — MS property accesses stay quiet; written subscripts keep the accessor's names

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

- [x] Explicit instantiation — an explicit instantiation definition adds no duplicate hints ([clangd#1034](https://github.com/clangd/clangd/issues/1034))

  ```cpp
  template <typename T>
  void apply(T value) {}

  template void apply<int>(int value);

  void use() {
      apply(42);
  }
  ```

- [ ] Sloppy name matching — `aParam` does not yet suppress an argument spelled `param` _(partial)_ ([clangd#2248](https://github.com/clangd/clangd/issues/2248))

  ```cpp
  void draw(int aParam);

  void use() {
      int param = 3;
      // Ideally the near-match would suppress the hint; today it still shows.
      draw(param);
  }
  ```

- [ ] Inherited constructors — `using Base::Base` calls lose their parameter names _(partial)_ ([clangd#1364](https://github.com/clangd/clangd/issues/1364))

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

- [x] Anonymous parameters — nothing to name, though a mutable reference still flags `&`

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

- [x] Operators and literals — operator syntax and user-defined literals stay bare; member and default member initializers hint

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

- [ ] Packs in constructor arguments — outer calls resolve; hints inside the expansion are still missing _(partial)_

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

<!-- END GENERATED ITEMS -->

## Type Hints

<!-- BEGIN GENERATED ITEMS: Type Hints -->

- [x] Deduced `auto` variables — the hint shows the full variable type, qualifiers included

  ```cpp
  int make();

  void use() {
      auto value = make();
      const auto& ref = value;
      auto* ptr = &value;
  }
  ```

- [x] Type sugar and the length limit — aliases keep their spelling; over-long types fall back to the sugared name ([clangd#1298](https://github.com/clangd/clangd/issues/1298), [clangd#1357](https://github.com/clangd/clangd/issues/1357))

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

- [x] Structured bindings — each binding hints its canonical type; the aggregate itself stays bare

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

- [x] Lambdas — variables, deduced return types, and init-captures all hint ([clangd#1163](https://github.com/clangd/clangd/issues/1163))

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

- [x] Deduced return types — `-> T` after the parameter list, declarations included

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

- [x] `decltype` spellings — the underlying type shows next to the written `decltype`

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

- [x] `auto` parameters — a template with exactly one instantiation reveals the deduced type

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

- [ ] Explicitly spelled initializers — casts and functional casts still hint redundantly _(partial)_ ([clangd#1749](https://github.com/clangd/clangd/issues/1749))

  ```cpp
  int compute();

  void use() {
      // The type is already written on the right-hand side; ideally these
      // two hints would be suppressed.
      auto widened = static_cast<long>(compute());
      auto braced = int{42};
  }
  ```

- [ ] Dependent `auto` — deduction inside an uninstantiated template body stays silent _(partial)_ ([clangd#2275](https://github.com/clangd/clangd/issues/2275))

  ```cpp
  template <typename T>
  void body(T input) {
      // No hint: the deduced type depends on T.
      auto derived = input + 1;
      // A dependence-free initializer still hints normally.
      auto counter = 0;
  }
  ```

- [x] Scope suppression — namespace qualifiers drop from hints; class scopes stay

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

- [x] Tuple-protocol bindings — hints print the canonical type, not `tuple_element<I, T>::type`

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

<!-- END GENERATED ITEMS -->

## Designator Hints

<!-- BEGIN GENERATED ITEMS: Designator Hints -->

- [x] Field and index designators — positional aggregate initialization shows `.field=` and `[index]=` ([clangd#2303](https://github.com/clangd/clangd/issues/2303))

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

- [x] Nested aggregates — written braces recurse; omitted braces flatten into `.outer.inner=`

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

- [x] Anonymous members — unnamed unions and structs vanish from the designator path

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

- [x] Designator suppression — written designators and `/*name=*/` comments keep their inits bare

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

- [x] Aggregates only — constructor calls, copies and idiomatic zero-init produce no designators

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

- [x] Broken initializers — designators survive next to initializers that fail to compile

  ```cpp
  // error-ok: the first initializer deliberately fails to convert.
  struct Empty {};

  struct Mixed {
      int a;
      int b;
  };

  void use() {
      Mixed m{Empty(), 1};
  }
  ```

- [ ] Parenthesized aggregate initialization — C++20 `Point(1, 2)` gets no hints yet ([clangd#2540](https://github.com/clangd/clangd/issues/2540))

  ```cpp
  struct Point {
      int x;
      int y;
  };

  Point p(1, 2);
  ```

<!-- END GENERATED ITEMS -->

## Other Hint Kinds

<!-- BEGIN GENERATED ITEMS: Other Hint Kinds -->

- [ ] Template parameter hints — deduced and explicit template arguments at call sites ([clangd#2583](https://github.com/clangd/clangd/issues/2583))

  ```cpp
  template <typename T, typename U>
  T convert(U val);

  // Could hint `T: float` next to the explicit argument list.
  float converted = convert<float>(42);
  ```

- [ ] CTAD arguments — deduced class template arguments after the template name ([clangd#2331](https://github.com/clangd/clangd/issues/2331))

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

- [ ] Implicit conversion hints — surface the conversions a call site performs ([clangd#2254](https://github.com/clangd/clangd/issues/2254))

  ```cpp
  void process(double val);

  // Could hint `(double)` before the argument.
  void use() {
      process(42);
  }
  ```

<!-- END GENERATED ITEMS -->

## Block End Hints

Off by default (`inlay_hints.block_end`). After the closing brace of a block spanning at least two lines, clice shows the name of what the brace closes — functions, types, namespaces, and control-flow statements:

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

Condition summaries print for `if`/`while`/`switch`/`for` where a short spelling exists; an `else if` chain hints as plain `// if`. Labels longer than 60 characters are dropped.

A related idea, `#endif` hints showing the matching condition ([clangd#2487](https://github.com/clangd/clangd/issues/2487)), is not implemented.

## Default Argument Hints

Off by default (`inlay_hints.default_arguments`). Call sites that rely on default arguments show what was omitted, abbreviated past the type-name limit:

```cpp
void log(int level, bool flush = true, int repeat = 1);
log(2);
//     ^ , flush: true, repeat: 1
```

## Configuration

The `[inlay_hints]` section of `clice.toml` (or the same keys via `initializationOptions`) controls every category: `enabled`, `parameters`, `deduced_types`, `designators`, `block_end`, `default_arguments`, and `type_name_limit`. See the [configuration guide](../guide/configuration.md#inlay-hints) for details. Configuration changes take effect after a server restart — a recompile is never involved.

## Interactive Behavior

- Requests are range-scoped: hints outside the requested range are discarded.
- Parameter hints anchor to the left of their argument; type and designator hints anchor to their declaration side with LSP padding flags instead of embedded spaces.
- Identical duplicate hints (e.g. from template instantiations) collapse into one.

## Other Known Gaps

- Abbreviated type hints with expandable label parts via `InlayHintLabelPart` ([clangd#2269](https://github.com/clangd/clangd/issues/2269))
- Clickable type names — go-to-definition on the hinted type ([clangd#1535](https://github.com/clangd/clangd/issues/1535))
- Scope-aware type shortening — print `Bar` instead of `foo::Bar` inside `namespace foo` ([clangd#2270](https://github.com/clangd/clangd/issues/2270))
- Parameter hints lost when a coroutine returns a template type ([clangd#2437](https://github.com/clangd/clangd/issues/2437))

## Changelog

| Date       | Change                                                                                                  | PR  |
| ---------- | ------------------------------------------------------------------------------------------------------- | --- |
| —          | Parameter name hints, type hints, range-scoped queries                                                  | —   |
| 2026-07-31 | Designator hints, dependent-call parameter hints, `[inlay_hints]` configuration, fixture-generated docs | —   |
