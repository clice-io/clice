# Inlay Hints

## Parameter Name Hints

- [x] Named parameter hints at call sites
- [x] Skip when argument name matches parameter name
- [x] Skip for single-parameter calls with obvious semantics
- [x] Expand parameter packs (`underlying_pack_type` detection)
- [x] Resolve parameter names from definition (not just declaration)
- [x] Forwarding function parameter resolution — show the underlying constructor parameters for `std::make_unique`, `emplace_back`, etc. ([clangd#2324](https://github.com/clangd/clangd/issues/2324))

  ```cpp
  struct Widget { Widget(int width, int height); };
  auto p = std::make_unique<Widget>(800, 600);
  //                                ^^^  ^^^
  //                          width: 800, height: 600 (not __args: 800, __args: 600)
  ```

- [x] Function pointer and `operator()` parameter hints ([clangd#1734](https://github.com/clangd/clangd/issues/1734), [clangd#1742](https://github.com/clangd/clangd/issues/1742))

  ```cpp
  void (*callback)(int status, const char* msg);
  callback(0, "ok");
  //       ^   ^^
  //  status: 0, msg: "ok"

  auto cmp = [](int a, int b) { return a < b; };
  cmp(1, 2);
  //  ^  ^
  //  a: 1, b: 2
  ```

- [ ] Template parameter hints — show deduced/explicit template arguments ([clangd#2583](https://github.com/clangd/clangd/issues/2583))

  ```cpp
  template<typename T, typename U>
  auto convert(U val) -> T;
  convert<float>(42);
  //      ^^^^^  ^^
  //   T: float, val: 42
  ```

- [ ] Case-insensitive parameter name matching — `aParam` should suppress hint for argument `param` ([clangd#2248](https://github.com/clangd/clangd/issues/2248))

- [x] Suppress hints when an inline comment already names the parameter ([clangd#1877](https://github.com/clangd/clangd/issues/1877))

  ```cpp
  draw(/*x=*/10, /*y=*/20);  // no hints needed — inline comments serve the same purpose
  ```

- [x] Default argument value hints

## Type Hints

- [x] `auto` deduced type hints
- [x] Structured binding type hints
- [x] Lambda return type hints
- [x] Range-based for loop variable type hints
- [ ] Lambda init-capture type hints ([clangd#1163](https://github.com/clangd/clangd/issues/1163))

  ```cpp
  auto f = [val = compute()] {};
  //        ^^^ : int
  ```

- [ ] Dependent `auto` type hints — show deduced type when useful even in template bodies ([clangd#2275](https://github.com/clangd/clangd/issues/2275))

- [x] Don't show type hint when deduction fails ([clangd#1475](https://github.com/clangd/clangd/issues/1475))

- [ ] Don't show type hint when the type is explicitly specified ([clangd#1749](https://github.com/clangd/clangd/issues/1749))

  ```cpp
  auto x = static_cast<int>(val);  // no type hint — already explicit
  auto y = int{42};                 // same
  ```

- [ ] Prefer desugared types — show `std::vector<int>` rather than a typedef alias ([clangd#1298](https://github.com/clangd/clangd/issues/1298), [clangd#1668](https://github.com/clangd/clangd/issues/1668))

  ```cpp
  using IntVec = std::vector<int>;
  IntVec create();
  auto v = create();
  //   ^ : std::vector<int>  (not IntVec)
  ```

- [x] Configurable type hint length limit ([clangd#1357](https://github.com/clangd/clangd/issues/1357))

- [ ] Abbreviated type hints with expandable label parts — use LSP `InlayHintLabelPart` to allow expanding truncated types ([clangd#2269](https://github.com/clangd/clangd/issues/2269))

  ```
  auto it = map.begin();
  //   ^^ : map<str…, int>::iterator  [click to expand]
  ```

- [ ] Shorten hints by suppressing obvious scopes ([clangd#2270](https://github.com/clangd/clangd/issues/2270))

  ```cpp
  // inside namespace foo:
  auto x = create();
  //   ^ : Bar  (not foo::Bar)
  ```

## Designator Hints

- [ ] Aggregate initializer designators (`.field =`) — show field names for positional aggregate initialization

  ```cpp
  struct Point { int x, y, z; };
  Point p = {1, 2, 3};
  //         ^  ^  ^
  //    .x = 1, .y = 2, .z = 3
  ```

- [ ] Parenthesized aggregate initialization (C++20) ([clangd#2540](https://github.com/clangd/clangd/issues/2540))

  ```cpp
  Point p(1, 2, 3);
  //      ^  ^  ^
  // .x = 1, .y = 2, .z = 3
  ```

- [ ] Compact array designator formatting ([clangd#2303](https://github.com/clangd/clangd/issues/2303))

  ```cpp
  int arr[] = {10, 20, 30};
  //           ^^  ^^  ^^
  //      [0]= 10, [1]= 20, [2]= 30
  ```

## Implicit Conversion Hints

- [ ] Show implicit type conversions at call sites and assignments ([clangd#2254](https://github.com/clangd/clangd/issues/2254))

  ```cpp
  void process(double val);
  process(42);
  //      ^^
  //      (double) 42  — implicit int→double conversion

  std::string s = "hello";
  //              ^^^^^^^
  //    (std::string) "hello"  — implicit const char*→string conversion
  ```

## Reference / Pointer Hints

- [x] Show `&` / `&&` to indicate the argument is passed by mutable reference ([clangd#1123](https://github.com/clangd/clangd/issues/1123))

  ```cpp
  void sort(std::vector<int>& v);
  sort(data);
  //   ^^^^
  //   &data  — passed by mutable reference
  ```

## Template Argument Hints

- [ ] CTAD deduced template arguments ([clangd#2331](https://github.com/clangd/clangd/issues/2331))

  ```cpp
  std::pair p(1, 3.14);
  //        ^ <int, double>
  ```

## Block End Hints

- [x] Show the declared name after a closing brace for long blocks ([clangd#1634](https://github.com/clangd/clangd/issues/1634))

  ```cpp
  void Widget::processData(const Config& cfg) {
      // ... 50+ lines ...
  } // Widget::processData
  ```

- [ ] `#endif` hints — show the corresponding macro condition ([clangd#2487](https://github.com/clangd/clangd/issues/2487))

  ```cpp
  #ifdef _WIN32
      // ...
  #endif // _WIN32
  ```

## Interactive Hints

- [x] Range-filtered results (hints outside the requested range are discarded; AST traversal is not range-limited)
- [x] Left-anchored hints (parameter names before argument)
- [x] Right-anchored hints (types after variable name)
- [ ] Clickable type names — go-to-definition on hinted type names via LSP `InlayHintLabelPart` ([clangd#1535](https://github.com/clangd/clangd/issues/1535))

  ```
  auto widget = create();
  //   ^^^^^^ : Widget  ← click "Widget" to go to Widget's definition
  ```

## Hint Correctness

Known issues that should be handled correctly:

- [ ] Nested macro invocations must not produce incorrect parameter name hints ([clangd#2620](https://github.com/clangd/clangd/issues/2620))
- [ ] No duplicate hints in the presence of explicit function template instantiation ([clangd#1034](https://github.com/clangd/clangd/issues/1034))
- [ ] Parameter hints for inherited constructors called from derived classes ([clangd#1364](https://github.com/clangd/clangd/issues/1364))
- [ ] Parameter hints must not be lost when a coroutine returns a template type ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- [x] C++23 deducing `this` — strip explicit object parameter from hint display ([clangd#1777](https://github.com/clangd/clangd/issues/1777))

## Changelog

| Date | Change                                                 | PR  |
| ---- | ------------------------------------------------------ | --- |
| —    | Parameter name hints, type hints, range-scoped queries | —   |
