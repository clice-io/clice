# Document Symbols

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/feature_docs.ts update`. -->

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`: a nested symbol tree with ranges, selection ranges and a `detail` field that disambiguates overloads and shows declared types.

## Symbol Hierarchy

<!-- BEGIN GENERATED ITEMS: Symbol Hierarchy -->

- [x] Nested symbol tree — symbols nest by their written scope; out-of-line definitions appear at their lexical position with qualified names

  <details>
  <summary>Example</summary>

  ```cpp
  namespace demo {

  struct Point {
      int x;
      int y;

      int manhattan() const;
  };

  int Point::manhattan() const {
      return x + y;
  }

  enum class Axis { X, Y };

  int origin_distance(const Point& p);

  namespace inner {
  constexpr int level = 2;
  }

  }  // namespace demo

  // A reopened namespace gets its own outline node per written scope.
  namespace demo {
  int reopened();
  }

  namespace demo::nested {
  int compact();
  }
  ```

  </details>

- [x] Symbol ranges and selection ranges — the range spans the whole declaration; the selection range covers the full written name, including multi-token names like `~Widget`, `operator==` and `operator bool`

  <details>
  <summary>Example</summary>

  ```cpp
  namespace members {

  struct Widget {
      Widget();
      explicit Widget(int size);
      ~Widget();

      Widget& operator=(const Widget& other);
      bool operator==(const Widget& other) const;
      operator bool() const;

      static int instances();

      int size;
      unsigned bits : 3;
      const char* name = "widget";
  };

  Widget::Widget(int size) : size(size), bits(0) {}

  int Widget::instances() {
      return 0;
  }

  }  // namespace members
  ```

  </details>

- [ ] Access specifier grouping — `public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation ([clangd#499](https://github.com/clangd/clangd/issues/499))

  <details>
  <summary>Example</summary>

  ```cpp
  class Widget {
  public:
      void draw();
      void resize();

  private:
      int width;
      int height;
  };
  ```

  </details>

- [x] Anonymous and inline scopes — anonymous namespaces, unnamed structs and unions group their members under a placeholder name; inline namespace members stay under the inline namespace node

  <details>
  <summary>Example</summary>

  ```cpp
  namespace {

  int hidden_counter = 0;

  }  // namespace

  namespace misc {

  inline namespace v1 {

  int versioned();

  }  // namespace v1

  struct Outer {
      struct {
          int anonymous_member;
      };

      union {
          int as_int;
          float as_float;
      };
  };

  }  // namespace misc
  ```

  </details>

- [x] UTF-16 position encoding — columns after non-ASCII text count UTF-16 code units

  <details>
  <summary>Example</summary>

  ```cpp
  // π ≈ 3.14159, 中文注释
  constexpr double 半径 = 2.0;
  constexpr double π值 = 3.14159; double area();
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Symbol Kinds

<!-- BEGIN GENERATED ITEMS: Symbol Kinds -->

- [x] Core symbol kinds — namespaces, classes, structs, unions, enums and their members, functions, variables, fields, structured bindings and lambdas all appear in the outline with a mapped LSP symbol kind

  <details>
  <summary>Example</summary>

  ```cpp
  namespace kinds {

  union Value {
      int i;
      float f;
  };

  enum Flags { FlagA, FlagB };

  enum class Mode : unsigned char { Fast, Safe };

  struct Pair {
      struct Meta {
          int tag;
      };

      int first;
      int second;
      static int instances;
  };

  Pair make_pair();

  auto [bound_first, bound_second] = make_pair();

  auto lambda = [](int x) {
      return x * 2;
  };

  }  // namespace kinds
  ```

  </details>

- [x] Template declarations — class, function and variable templates carry a `template ` detail prefix; concepts and abbreviated function templates (`concept auto` parameters) appear as well

  <details>
  <summary>Example</summary>

  ```cpp
  namespace templates {

  template <typename T>
  struct Box {
      T value;

      void reset();
  };

  template <typename T>
  void Box<T>::reset() {}

  template <typename T>
  T zero() {
      return T();
  }

  template <typename T>
  constexpr T pi = T(3.14159);

  template <typename T>
  concept Small = sizeof(T) <= 4;

  void takes_concept(Small auto x);

  }  // namespace templates
  ```

  </details>

- [x] Template specializations and deduction guides — explicit and partial specializations of class and variable templates appear with their template arguments in the name; members nest under their specialization; deduction guides render their deduced signature

  <details>
  <summary>Example</summary>

  ```cpp
  namespace spec {

  template <typename T>
  struct Box {
      T value;
  };

  template <>
  struct Box<void> {};

  template <typename T>
  struct Box<T*> {
      T* pointee;
  };

  template <typename T>
  T zero() {
      return T();
  }

  template <>
  int zero<int>();

  template <typename T>
  constexpr T pi = T(3);

  template <>
  constexpr int pi<int> = 3;

  template <typename T>
  constexpr T* pi<T*> = nullptr;

  template <typename T>
  struct Deduced {
      Deduced(T raw);
  };

  template <typename T>
  Deduced(T*) -> Deduced<T>;

  // Forces the implicit instantiation Box<int>, which must not appear.
  Box<int> instantiated;

  // An explicit class instantiation gets a childless node; the instantiated
  // members and the function instantiation (whose location clang records at
  // the primary) produce no symbols.
  template struct Box<char>;
  template long zero<long>();

  }  // namespace spec
  ```

  </details>

- [x] Type aliases — `typedef`, `using` aliases and alias templates appear in the outline with a `type alias` detail

  <details>
  <summary>Example</summary>

  ```cpp
  namespace aliases {

  struct Widget {};

  typedef Widget LegacyWidget;

  using ModernWidget = Widget;

  template <typename T>
  struct Box {};

  template <typename T>
  using BoxOf = Box<T>;

  struct Holder {
      using Inner = Widget;
  };

  }  // namespace aliases
  ```

  </details>

- [ ] Explicit instantiation directives — the class forms appear as childless symbols; clang mislocates the function and variable forms at the pattern, so they are missing from the outline _(partial)_ ([llvm#191658](https://github.com/llvm/llvm-project/issues/191658))

  <details>
  <summary>Example</summary>

  ```cpp
  template <typename T>
  struct Box {
      T value;
  };

  template struct Box<int>;
  extern template struct Box<char>;

  template <typename T>
  void convert(T value) {}

  template void convert<int>(int);

  template <typename T>
  T zero = T();

  template int zero<int>;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Symbol Detail

<!-- BEGIN GENERATED ITEMS: Symbol Detail -->

- [x] Function signatures — parameter and return types in the `detail` field disambiguate overloads; constructors drop the `void` return type ([clangd#520](https://github.com/clangd/clangd/issues/520), [clangd#601](https://github.com/clangd/clangd/issues/601), [clangd#1232](https://github.com/clangd/clangd/issues/1232))

  <details>
  <summary>Example</summary>

  ```cpp
  namespace detail {

  void process(int x);
  void process(const char* s);

  struct Task {
      Task();
      Task(int priority);

      int run(bool async) const;
  };

  }  // namespace detail
  ```

  </details>

- [x] Variable and field types — the declared type in the `detail` field; lambdas render as `(lambda)`

  <details>
  <summary>Example</summary>

  ```cpp
  namespace detail {

  int timeout = 30;
  const char* logger_name = "core";

  struct Config {
      unsigned retries;
      double backoff;
  };

  auto on_error = [](int code) {
      return code != 0;
  };

  }  // namespace detail
  ```

  </details>

- [x] Default argument stripping — the signature is derived from the function type, so default parameter values never leak into the outline ([clangd#221](https://github.com/clangd/clangd/issues/221))

  <details>
  <summary>Example</summary>

  ```cpp
  namespace detail {

  void open_file(const char* path, int mode = 0644);

  struct Server {
      void listen(int port = 8080, int backlog = 128);
  };

  }  // namespace detail
  ```

  </details>

- [ ] Base classes in detail — show `: Shape` on derived class declarations

  <details>
  <summary>Example</summary>

  ```cpp
  struct Shape {};

  struct Circle : Shape {
      double radius;
  };
  ```

  </details>

- [x] Multiline signature ranges — the symbol range starts at the beginning of the declaration and spans the full signature, so editor sticky scroll anchors correctly ([clangd#2221](https://github.com/clangd/clangd/issues/2221))

  <details>
  <summary>Example</summary>

  ```cpp
  struct Config {};

  void process_data(
      const Config& cfg,
      int flags
  ) {}
  ```

  </details>

- [x] Scoped types — a written class scope appears in the detail exactly once, for nested classes, template-ids, aliases and dependent names alike

  <details>
  <summary>Example</summary>

  ```cpp
  namespace scoped {

  struct Outer {
      struct Inner {};
      template <typename T> struct Box {};
      using Alias = int;
  };

  struct User {
      Outer::Inner plain;
      Outer::Box<int> boxed;
      Outer::Alias aliased;
      const Outer::Inner frozen;
  };

  template <typename T>
  struct Holder {
      typename T::type value;
      typename T::inner::type deep;
      typename T::template rebind<int> bound;
  };

  }  // namespace scoped
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Missing Symbols

<!-- BEGIN GENERATED ITEMS: Missing Symbols -->

- [ ] Macro definitions — object-like and function-like macro definitions in the outline ([clangd#1744](https://github.com/clangd/clangd/issues/1744))

  <details>
  <summary>Example</summary>

  ```cpp
  #define MAX_BUFFER_SIZE 4096
  #define CHECK(cond, msg) ((cond) ? 0 : (msg))
  ```

  </details>

- [ ] Include directives — `#include` entries in the outline ([clangd#2226](https://github.com/clangd/clangd/issues/2226))

  <details>
  <summary>Example</summary>

  ```cpp
  #include "config.h"

  int uses_config();
  ```

  </details>

- [x] Local symbols — variables and types declared inside function bodies nest under their function ([clangd#616](https://github.com/clangd/clangd/issues/616))

  <details>
  <summary>Example</summary>

  ```cpp
  int compute() {
      int local_sum = 0;

      struct Accumulator {
          int total;
      };

      auto twice = [](int x) {
          return 2 * x;
      };

      struct Pair {
          int a;
          int b;
      };

      auto [first, second] = Pair{1, 2};

      return local_sum + twice(first) + second;
  }
  ```

  </details>

- [ ] Module declarations — `export module`, `module` and `import` declarations in the outline

  <details>
  <summary>Example</summary>

  ```cpp
  export module app.core;

  import std;

  export int core_entry();
  ```

  </details>

- [ ] `#pragma mark` navigation markers — editor section markers as outline entries

  <details>
  <summary>Example</summary>

  ```cpp
  #pragma mark - Lifecycle

  void setup();

  #pragma mark - Rendering

  void draw();
  ```

  </details>

- [x] Friend function definitions — a friend function defined inline in a class appears under that class

  <details>
  <summary>Example</summary>

  ```cpp
  struct Owner {
      friend void inline_friend(Owner& o) {}

      friend bool operator==(const Owner& lhs, const Owner& rhs) {
          return &lhs == &rhs;
      }
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Symbol Tags

<!-- BEGIN GENERATED ITEMS: Symbol Tags -->

- [ ] Deprecated tag — mark `[[deprecated]]` symbols with the LSP `deprecated` symbol tag

  <details>
  <summary>Example</summary>

  ```cpp
  [[deprecated("use open_v2")]] void open_v1();

  void open_v2();
  ```

  </details>

- [ ] Access and storage indicators — public / private / protected, static, virtual and abstract markers on outline entries ([clangd#2123](https://github.com/clangd/clangd/issues/2123))

  <details>
  <summary>Example</summary>

  ```cpp
  class Base {
  public:
      virtual void render() = 0;

  protected:
      static int instances();

  private:
      int id;
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Location Correctness

<!-- BEGIN GENERATED ITEMS: Location Correctness -->

- [x] Symbols from macro expansions — a symbol produced by a macro invocation is located at the invocation, not at the macro definition ([clangd#475](https://github.com/clangd/clangd/issues/475))

  <details>
  <summary>Example</summary>

  ```cpp
  #define DEFINE_HANDLER(name) void name()

  DEFINE_HANDLER(on_ready);
  DEFINE_HANDLER(on_close);

  #define DECLARE_CLASS(X) class X
  DECLARE_CLASS(Generated) {
      int member;
  };
  ```

  </details>

- [x] Names spelled in macro arguments — the selection range points at the name written in the macro argument; names spelled in the macro body fall back to the invocation site ([clangd#1941](https://github.com/clangd/clangd/issues/1941))

  <details>
  <summary>Example</summary>

  ```cpp
  #define VAR(X) int X = 1;

  VAR(from_argument)

  #define COUNTER() int counter_from_body = 0;

  COUNTER()
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Changelog

| Date       | Change                                                                                                                                  | PR                                                 |
| ---------- | --------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| 2026-08-01 | Explicit instantiation directives pinned: class forms as childless symbols, function and variable forms missing until clang 23          | [#571](https://github.com/clice-io/clice/pull/571) |
| 2026-08-01 | Template specializations, type aliases, full-name selection ranges, macro-argument names; traversal moved onto the semantics node table | [#566](https://github.com/clice-io/clice/pull/566) |
| 2025-01-13 | Nested symbol hierarchy, basic symbol kinds                                                                                             | [#17](https://github.com/clice-io/clice/pull/17)   |
