# Hover

## Symbol Information

- [x] Qualified name showing enclosing scope context (namespace, class)
- [x] Symbol kind classification
- [x] Access specifier (public / protected / private)
- [x] Documentation comments (Doxygen)
- [x] Source definition rendering
- [x] Truncate large initializers in definition display ([clangd#710](https://github.com/clangd/clangd/issues/710))

  ```cpp
  const int table[] = {0, 1, 2, /* ... 1000 elements ... */};
  // hover → show truncated definition, not the entire initializer
  ```

- [ ] Show `virtual` / `override` / `final` modifiers in hover ([clangd#2474](https://github.com/clangd/clangd/issues/2474))

  ```cpp
  struct Base {
      virtual void draw() = 0;
  };
  struct Circle : Base {
      void draw() override;  // hover should show "virtual void draw() override"
  };
  ```

- [ ] Show anonymous namespace in scope display ([clangd#436](https://github.com/clangd/clangd/issues/436))

  ```cpp
  namespace { void helper(); }
  // hover on helper → scope: "(anonymous namespace)::"
  ```

## Type Information

- [x] Variable type with pretty-printing
- [x] Desugared type (`aka` field) for type aliases
- [x] Return type for functions/lambdas
- [x] Function parameters with types, names, defaults
- [x] Template parameters
- [x] Deduced type on `auto` and `decltype` keyword hover
- [ ] Show deduced template arguments for CTAD variables ([clangd#435](https://github.com/clangd/clangd/issues/435))

  ```cpp
  std::pair p(1, 3.14);
  // hover on p → "std::pair<int, double> p"
  ```

- [ ] Show template arguments for instantiations ([clangd#230](https://github.com/clangd/clangd/issues/230))

  ```cpp
  template<typename T> T identity(T x) { return x; }
  identity(42);
  // hover → show "T = int" in template parameters
  ```

- [ ] Resolve lambda `auto` parameter types from call context ([clangd#493](https://github.com/clangd/clangd/issues/493))

  ```cpp
  auto cmp = [](auto a, auto b) { return a < b; };
  std::sort(v.begin(), v.end(), cmp);
  // hover on a → "int" (deduced from std::sort<int>)
  ```

- [ ] Preserve sugared types for `auto` variables ([clangd#709](https://github.com/clangd/clangd/issues/709))

  ```cpp
  auto it = vec.begin();
  // current:  "auto it" → "__gnu_cxx::__normal_iterator<int*, ...>"
  // expected: "auto it" → "std::vector<int>::iterator"
  ```

- [ ] Apply `.clang-format` style to printed type names ([clangd#2156](https://github.com/clangd/clangd/issues/2156))

  ```cpp
  // .clang-format: QualifierAlignment: Right
  const int* p;
  // current:  hover → "const int *p"
  // expected: hover → "int const *p"
  ```

- [ ] Correct hover for C typedef of anonymous struct ([clangd#2219](https://github.com/clangd/clangd/issues/2219))

  ```cpp
  typedef struct { int x, y; } Point;
  Point p;
  // current:  hover → "struct Point p" (misleading — no struct Point exists)
  // expected: hover → "Point p" (alias to anonymous struct)
  ```

- [ ] Concept and constrained `auto` hover showing constraint information

## Layout Information

- [x] Size (bits) for fields and types
- [x] Offset within enclosing class
- [x] Padding detection
- [x] Alignment
- [ ] Show alignment and padding for type-level hover, not just fields ([clangd#1763](https://github.com/clangd/clangd/issues/1763))

  ```cpp
  struct Widget {
      int id;       // offset 0, size 32
      double value; // offset 64, size 64, padding 32
  };
  // hover on Widget → size: 128 bits, align: 64 bits, padding: 32 bits total
  ```

- [ ] Virtual function table offset for class methods ([clangd#1771](https://github.com/clangd/clangd/issues/1771))

  ```cpp
  struct Base {
      virtual void foo();  // hover → vtable offset: 0
      virtual void bar();  // hover → vtable offset: 1
  };
  ```

## Expression Context

- [x] Evaluated constant value (`value` field)
- [x] Callee argument info (which parameter this maps to)
- [x] Pass-by semantics (ref, const ref, value)
- [x] Implicit conversion detection
- [ ] Show string literal length on hover ([clangd#1016](https://github.com/clangd/clangd/issues/1016))

  ```cpp
  const char* msg = "hello world";
  //                 ^^^^^^^^^^^^^ hover → length: 11 (excluding null terminator)
  ```

- [ ] Show numeric literal type on hover ([clangd#1669](https://github.com/clangd/clangd/issues/1669))

  ```cpp
  auto x = 42ULL;
  //       ^^^^^ hover → type: unsigned long long, value: 42
  auto y = 0x1p10;
  //       ^^^^^^ hover → type: double, value: 1024.0
  ```

- [ ] Avoid misleading expression value when hovering over record-type variables ([clangd#1622](https://github.com/clangd/clangd/issues/1622))

  ```cpp
  constexpr int N = 10;
  std::array<int, N> arr;
  arr[0];
  // hover on arr → should NOT show "value: 10" from enclosing expression
  ```

## Documentation

- [ ] Resolve `@copydoc` Doxygen tags ([clangd#1320](https://github.com/clangd/clangd/issues/1320))

  ```cpp
  /// Detailed documentation.
  void base_func();

  /// @copydoc base_func()
  void wrapper();
  // hover on wrapper → show base_func's documentation
  ```

- [ ] Inherit documentation from overridden base methods ([clangd#2504](https://github.com/clangd/clangd/issues/2504))

  ```cpp
  struct Base {
      /// Renders the widget.
      virtual void draw();
  };
  struct Circle : Base {
      void draw() override;
      // hover → "Renders the widget." (inherited from Base::draw)
  };
  ```

- [ ] Share documentation across consecutive overloads ([clangd#2506](https://github.com/clangd/clangd/issues/2506))

  ```cpp
  /// Opens a file.
  void open(const char* path);
  void open(const char* path, int flags);
  // hover on second overload → "Opens a file." (shared from first)
  ```

- [ ] Documentation from inherited constructors ([clangd#1936](https://github.com/clangd/clangd/issues/1936))

- [ ] Only associate comments directly above the declaration ([clangd#974](https://github.com/clangd/clangd/issues/974))

  ```cpp
  // ==== Section Banner ====

  void foo();
  // hover on foo → should NOT show "==== Section Banner ===="
  ```

- [ ] Option to suppress misattributed doc comments ([clangd#2148](https://github.com/clangd/clangd/issues/2148))

- [ ] Prefer declaration documentation over definition comments ([clangd#829](https://github.com/clangd/clangd/issues/829))

  ```cpp
  // header.h:
  /// Public API documentation.
  void process(int x);

  // source.cpp:
  // internal implementation note
  void process(int x) { ... }
  // hover → show "Public API documentation.", not "internal implementation note"
  ```

- [ ] Preserve whitespace and newlines from comments ([clangd#2057](https://github.com/clangd/clangd/issues/2057))

  ```cpp
  /// | Column A | Column B |
  /// |----------|----------|
  /// | 1        | 2        |
  void table_fn();
  // hover → render the markdown table correctly, not as a single line
  ```

- [ ] Fix wrong indentation in hover comment rendering ([clangd#1040](https://github.com/clangd/clangd/issues/1040))

- [ ] Docstrings missing when template keyword comes from macro expansion ([clangd#1226](https://github.com/clangd/clangd/issues/1226))

- [x] Synthesized documentation for trivial accessors (getters/setters)

## Macro Hover

- [ ] Show macro expansion before definition ([clangd#2642](https://github.com/clangd/clangd/issues/2642))

  ```cpp
  #define MAX(a, b) ((a) > (b) ? (a) : (b))
  int z = MAX(x, y);
  //      ^^^^^^^^^ hover → expansion: "((x) > (y) ? (x) : (y))"
  //                         definition: "#define MAX(a, b) ((a) > (b) ? (a) : (b))"
  ```

## Special Hover Targets

- [x] Show struct/enum members on type-level hover ([clangd#959](https://github.com/clangd/clangd/issues/959))

  ```cpp
  enum Color { Red, Green, Blue };
  // hover on Color → "enum Color { Red, Green, Blue }"
  ```

- [ ] Show underlying struct definition for typedef ([clangd#2020](https://github.com/clangd/clangd/issues/2020))

  ```cpp
  typedef struct { int x, y; } Point;
  Point p;
  // hover on Point → show the struct definition including members
  ```

- [ ] Keyword documentation on hover ([clangd#1862](https://github.com/clangd/clangd/issues/1862))

  ```cpp
  const int x = 42;
  // hover on "const" → description of the keyword
  ```

- [x] Attribute documentation on hover ([clangd#1862](https://github.com/clangd/clangd/issues/1862))

  ```cpp
  [[nodiscard]] int compute();
  // hover on "nodiscard" → description of the attribute
  ```

- [x] `#include` directive hover showing resolved header path
- [x] `this` expression hover showing pointed-to type
- [x] `__func__` and related predefined identifier hover

- [ ] GTK-Doc, kernel-doc, and GObject Introspection documentation ([clangd#2662](https://github.com/clangd/clangd/issues/2662))

- [ ] LaTeX rendering of Doxygen `@f$` math formulas ([clangd#2669](https://github.com/clangd/clangd/issues/2669))

## Module-Related

- [ ] Hover on `import` statement shows module info
- [ ] Hover on module name shows owning file/partition list

## Hover Correctness

Known issues that should be handled correctly:

- [ ] `MSInheritanceAttr` causing incorrect hover on MSVC targets ([clangd#1643](https://github.com/clangd/clangd/issues/1643), [clangd#2212](https://github.com/clangd/clangd/issues/2212))
- [ ] Object instantiation misidentified as function declaration ([clangd#2225](https://github.com/clangd/clangd/issues/2225))
- [ ] Crash on large unsigned enum constant value ([clangd#2381](https://github.com/clangd/clangd/issues/2381))
- [ ] Crash on function call hover with default arguments ([clangd#551](https://github.com/clangd/clangd/issues/551))
- [ ] Symbols shadowed by function-like macros with the same name ([clangd#2490](https://github.com/clangd/clangd/issues/2490))

## Changelog

| Date    | Change                           | PR     |
| ------- | -------------------------------- | ------ |
| 2025-06 | Port clangd hover implementation | [#452] |

[#452]: https://github.com/clice-io/clice/pull/452
