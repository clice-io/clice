# Document Symbols

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`: a nested symbol tree with ranges, selection ranges and a `detail` field that disambiguates overloads and shows declared types.

## Symbol Hierarchy

<!-- BEGIN GENERATED ITEMS: symbol_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Nested symbol tree**

Symbols nest by their written scope; out-of-line definitions appear at their lexical position with qualified names

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "demo", kind: Namespace, range: "4:0-25:1", selection_range: "4:10-4:14" }
  -   { name: "Point", kind: Struct, range: "6:0-11:1", selection_range: "6:7-6:12", detail: "struct" }
  -     { name: "x", kind: Field, range: "7:4-7:9", selection_range: "7:8-7:9", detail: "int" }
  -     { name: "y", kind: Field, range: "8:4-8:9", selection_range: "8:8-8:9", detail: "int" }
  -     { name: "manhattan", kind: Method, range: "10:4-10:25", selection_range: "10:8-10:17", detail: "int () const" }
  -   { name: "Point::manhattan", kind: Method, range: "13:0-15:1", selection_range: "13:11-13:20", detail: "int () const" }
  -   { name: "Axis", kind: Enum, range: "17:0-17:24", selection_range: "17:11-17:15", detail: "enum" }
  -     { name: "X", kind: EnumMember, range: "17:18-17:19", selection_range: "17:18-17:19", detail: "Axis" }
  -     { name: "Y", kind: EnumMember, range: "17:21-17:22", selection_range: "17:21-17:22", detail: "Axis" }
  -   { name: "origin_distance", kind: Function, range: "19:0-19:35", selection_range: "19:4-19:19", detail: "int (const Point &)" }
  -   { name: "inner", kind: Namespace, range: "21:0-23:1", selection_range: "21:10-21:15" }
  -     { name: "level", kind: Variable, range: "22:0-22:23", selection_range: "22:14-22:19", detail: "const int" }
  - { name: "demo", kind: Namespace, range: "28:0-30:1", selection_range: "28:10-28:14" }
  -   { name: "reopened", kind: Function, range: "29:0-29:14", selection_range: "29:4-29:12", detail: "int ()" }
  - { name: "demo", kind: Namespace, range: "32:0-34:1", selection_range: "32:10-32:14" }
  -   { name: "nested", kind: Namespace, range: "32:14-34:1", selection_range: "32:16-32:22" }
  -     { name: "compact", kind: Function, range: "33:0-33:13", selection_range: "33:4-33:11", detail: "int ()" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Symbol ranges and selection ranges**

The range spans the whole declaration; the selection range covers the full written name, including multi-token names like `~Widget`, `operator==` and `operator bool`

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "members", kind: Namespace, range: "4:0-28:1", selection_range: "4:10-4:17" }
  -   { name: "Widget", kind: Struct, range: "6:0-20:1", selection_range: "6:7-6:13", detail: "struct" }
  -     { name: "Widget", kind: Method, range: "7:4-7:12", selection_range: "7:4-7:10", detail: "()" }
  -     { name: "Widget", kind: Method, range: "8:4-8:29", selection_range: "8:13-8:19", detail: "(int)" }
  -     { name: "~Widget", kind: Method, range: "9:4-9:13", selection_range: "9:4-9:11" }
  -     { name: "operator=", kind: Operator, range: "11:4-11:42", selection_range: "11:12-11:21", detail: "Widget &(const Widget &)" }
  -     { name: "operator==", kind: Operator, range: "12:4-12:46", selection_range: "12:9-12:19", detail: "bool (const Widget &) const" }
  -     { name: "operator bool", kind: Method, range: "13:4-13:25", selection_range: "13:4-13:17", detail: "bool () const" }
  -     { name: "instances", kind: Method, range: "15:4-15:26", selection_range: "15:15-15:24", detail: "int ()" }
  -     { name: "size", kind: Field, range: "17:4-17:12", selection_range: "17:8-17:12", detail: "int" }
  -     { name: "bits", kind: Field, range: "18:4-18:21", selection_range: "18:13-18:17", detail: "unsigned int" }
  -     { name: "name", kind: Field, range: "19:4-19:31", selection_range: "19:16-19:20", detail: "const char *" }
  -   { name: "Widget::Widget", kind: Method, range: "22:0-22:49", selection_range: "22:8-22:14", detail: "(int)" }
  -   { name: "Widget::instances", kind: Method, range: "24:0-26:1", selection_range: "24:12-24:21", detail: "int ()" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#499 -->

**Access specifier grouping**

`public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation

```snap-document_symbol
feature: document_symbol
code: |
  class Widget {
  public:
      void draw();
      void resize();

  private:
      int width;
      int height;
  };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous and inline scopes**

Anonymous namespaces, unnamed structs and unions group their members under a placeholder name; inline namespace members stay under the inline namespace node

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "(anonymous namespace)", kind: Namespace, range: "4:0-8:1", selection_range: "4:10-4:11" }
  -   { name: "hidden_counter", kind: Variable, range: "6:0-6:22", selection_range: "6:4-6:18", detail: "int" }
  - { name: "misc", kind: Namespace, range: "10:0-29:1", selection_range: "10:10-10:14" }
  -   { name: "v1", kind: Namespace, range: "12:0-16:1", selection_range: "12:17-12:19" }
  -     { name: "versioned", kind: Function, range: "14:0-14:15", selection_range: "14:4-14:13", detail: "int ()" }
  -   { name: "Outer", kind: Struct, range: "18:0-27:1", selection_range: "18:7-18:12", detail: "struct" }
  -     { name: "(anonymous struct)", kind: Struct, range: "19:4-21:5", selection_range: "19:4-19:10", detail: "struct" }
  -       { name: "anonymous_member", kind: Field, range: "20:8-20:28", selection_range: "20:12-20:28", detail: "int" }
  -     { name: "(anonymous union)", kind: Class, range: "23:4-26:5", selection_range: "23:4-23:9", detail: "union" }
  -       { name: "as_int", kind: Field, range: "24:8-24:18", selection_range: "24:12-24:18", detail: "int" }
  -       { name: "as_float", kind: Field, range: "25:8-25:22", selection_range: "25:14-25:22", detail: "float" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**UTF-16 position encoding**

Columns after non-ASCII text count UTF-16 code units

```snap-document_symbol
feature: document_symbol
code: |
  // π ≈ 3.14159, 中文注释
  constexpr double 半径 = 2.0;
  constexpr double π值 = 3.14159; double area();
snapshot: |
  - { name: "半径", kind: Variable, range: "5:0-5:25", selection_range: "5:17-5:19", detail: "const double" }
  - { name: "π值", kind: Variable, range: "6:0-6:29", selection_range: "6:17-6:19", detail: "const double" }
  - { name: "area", kind: Function, range: "6:31-6:44", selection_range: "6:38-6:42", detail: "double ()" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Kinds

<!-- BEGIN GENERATED ITEMS: symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Core symbol kinds**

namespaces, classes, structs, unions, enums and their members, functions, variables, fields, structured bindings and lambdas all appear in the outline with a mapped LSP symbol kind

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "kinds", kind: Namespace, range: "4:0-33:1", selection_range: "4:10-4:15" }
  -   { name: "Value", kind: Class, range: "6:0-9:1", selection_range: "6:6-6:11", detail: "union" }
  -     { name: "i", kind: Field, range: "7:4-7:9", selection_range: "7:8-7:9", detail: "int" }
  -     { name: "f", kind: Field, range: "8:4-8:11", selection_range: "8:10-8:11", detail: "float" }
  -   { name: "Flags", kind: Enum, range: "11:0-11:27", selection_range: "11:5-11:10", detail: "enum" }
  -     { name: "FlagA", kind: EnumMember, range: "11:13-11:18", selection_range: "11:13-11:18", detail: "Flags" }
  -     { name: "FlagB", kind: EnumMember, range: "11:20-11:25", selection_range: "11:20-11:25", detail: "Flags" }
  -   { name: "Mode", kind: Enum, range: "13:0-13:46", selection_range: "13:11-13:15", detail: "enum" }
  -     { name: "Fast", kind: EnumMember, range: "13:34-13:38", selection_range: "13:34-13:38", detail: "Mode" }
  -     { name: "Safe", kind: EnumMember, range: "13:40-13:44", selection_range: "13:40-13:44", detail: "Mode" }
  -   { name: "Pair", kind: Struct, range: "15:0-23:1", selection_range: "15:7-15:11", detail: "struct" }
  -     { name: "Meta", kind: Struct, range: "16:4-18:5", selection_range: "16:11-16:15", detail: "struct" }
  -       { name: "tag", kind: Field, range: "17:8-17:15", selection_range: "17:12-17:15", detail: "int" }
  -     { name: "first", kind: Field, range: "20:4-20:13", selection_range: "20:8-20:13", detail: "int" }
  -     { name: "second", kind: Field, range: "21:4-21:14", selection_range: "21:8-21:14", detail: "int" }
  -     { name: "instances", kind: Variable, range: "22:4-22:24", selection_range: "22:15-22:24", detail: "int" }
  -   { name: "make_pair", kind: Function, range: "25:0-25:16", selection_range: "25:5-25:14", detail: "Pair ()" }
  -   { name: "bound_first", kind: Variable, range: "27:6-27:17", selection_range: "27:6-27:17", detail: "int" }
  -   { name: "bound_second", kind: Variable, range: "27:19-27:31", selection_range: "27:19-27:31", detail: "int" }
  -   { name: "lambda", kind: Variable, range: "29:0-31:1", selection_range: "29:5-29:11", detail: "(lambda)" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template declarations**

class, function and variable templates carry a `template ` detail prefix; concepts and abbreviated function templates (`concept auto` parameters) appear as well

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "templates", kind: Namespace, range: "4:0-29:1", selection_range: "4:10-4:19" }
  -   { name: "Box", kind: Struct, range: "7:0-11:1", selection_range: "7:7-7:10", detail: "template struct" }
  -     { name: "value", kind: Field, range: "8:4-8:11", selection_range: "8:6-8:11", detail: "T" }
  -     { name: "reset", kind: Method, range: "10:4-10:16", selection_range: "10:9-10:14", detail: "void ()" }
  -   { name: "Box<T>::reset", kind: Method, range: "13:0-14:23", selection_range: "14:13-14:18", detail: "void ()" }
  -   { name: "zero", kind: Function, range: "17:0-19:1", selection_range: "17:2-17:6", detail: "template T ()" }
  -   { name: "pi", kind: Variable, range: "22:0-22:27", selection_range: "22:12-22:14", detail: "template const T" }
  -   { name: "Small", kind: TypeParameter, range: "24:0-25:30", selection_range: "25:8-25:13", detail: "concept" }
  -   { name: "takes_concept", kind: Function, range: "27:0-27:32", selection_range: "27:5-27:18", detail: "template void (Small auto)" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template specializations and deduction guides**

Explicit and partial specializations of class and variable templates appear with their template arguments in the name; members nest under their specialization; deduction guides render their deduced signature

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "spec", kind: Namespace, range: "4:0-53:1", selection_range: "4:10-4:14" }
  -   { name: "Box", kind: Struct, range: "7:0-9:1", selection_range: "7:7-7:10", detail: "template struct" }
  -     { name: "value", kind: Field, range: "8:4-8:11", selection_range: "8:6-8:11", detail: "T" }
  -   { name: "Box<void>", kind: Struct, range: "11:0-12:19", selection_range: "12:7-12:10", detail: "struct" }
  -   { name: "Box<T *>", kind: Struct, range: "14:0-17:1", selection_range: "15:7-15:10", detail: "template struct" }
  -     { name: "pointee", kind: Field, range: "16:4-16:14", selection_range: "16:7-16:14", detail: "T *" }
  -   { name: "zero", kind: Function, range: "20:0-22:1", selection_range: "20:2-20:6", detail: "template T ()" }
  -   { name: "zero<int>", kind: Function, range: "24:0-25:15", selection_range: "25:4-25:8", detail: "int ()" }
  -   { name: "pi", kind: Variable, range: "28:0-28:21", selection_range: "28:12-28:14", detail: "template const T" }
  -   { name: "pi<int>", kind: Variable, range: "30:0-31:25", selection_range: "31:14-31:16", detail: "const int" }
  -   { name: "pi<T *>", kind: Variable, range: "33:0-34:29", selection_range: "34:13-34:15", detail: "template T *const" }
  -   { name: "Deduced", kind: Struct, range: "37:0-39:1", selection_range: "37:7-37:14", detail: "template struct" }
  -     { name: "Deduced", kind: Method, range: "38:4-38:18", selection_range: "38:4-38:11", detail: "(T)" }
  -   { name: "<deduction guide for Deduced>", kind: Function, range: "42:0-42:25", selection_range: "42:0-42:7", detail: "template auto (T *) -> Deduced<T>" }
  -   { name: "instantiated", kind: Variable, range: "45:0-45:21", selection_range: "45:9-45:21", detail: "Box<int>" }
  -   { name: "Box<char>", kind: Struct, range: "50:0-50:25", selection_range: "50:16-50:19", detail: "struct" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

`typedef`, `using` aliases and alias templates appear in the outline with a `type alias` detail

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "aliases", kind: Namespace, range: "4:0-22:1", selection_range: "4:10-4:17" }
  -   { name: "Widget", kind: Struct, range: "6:0-6:16", selection_range: "6:7-6:13", detail: "struct" }
  -   { name: "LegacyWidget", kind: Class, range: "8:0-8:27", selection_range: "8:15-8:27", detail: "type alias" }
  -   { name: "ModernWidget", kind: Class, range: "10:0-10:27", selection_range: "10:6-10:18", detail: "type alias" }
  -   { name: "Box", kind: Struct, range: "13:0-13:13", selection_range: "13:7-13:10", detail: "template struct" }
  -   { name: "BoxOf", kind: Class, range: "16:0-16:20", selection_range: "16:6-16:11", detail: "template type alias" }
  -   { name: "Holder", kind: Struct, range: "18:0-20:1", selection_range: "18:7-18:13", detail: "struct" }
  -     { name: "Inner", kind: Class, range: "19:4-19:24", selection_range: "19:10-19:15", detail: "type alias" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Explicit instantiation directives**

The class forms appear as childless symbols; clang mislocates the function and variable forms at the pattern, so they are missing from the outline

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "Box", kind: Struct, range: "6:0-8:1", selection_range: "6:7-6:10", detail: "template struct" }
  -   { name: "value", kind: Field, range: "7:4-7:11", selection_range: "7:6-7:11", detail: "T" }
  - { name: "Box<int>", kind: Struct, range: "10:0-10:24", selection_range: "10:16-10:19", detail: "struct" }
  - { name: "Box<char>", kind: Struct, range: "11:0-11:32", selection_range: "11:23-11:26", detail: "struct" }
  - { name: "convert", kind: Function, range: "14:0-14:24", selection_range: "14:5-14:12", detail: "template void (T)" }
  - { name: "zero", kind: Variable, range: "19:0-19:12", selection_range: "19:2-19:6", detail: "template T" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**Macro definitions**

object-like and function-like macro definitions in the outline, a parameter list as the function-like detail

```snap-document_symbol
feature: document_symbol
code: |
  // The assertion holds the directives out of the preamble region, whose
  // live record the server path does not yet see.
  static_assert(true);

  #define MAX_BUFFER_SIZE 4096
  #define CHECK(cond, msg) ((cond) ? 0 : (msg))
  #define TRACE(...) log(__VA_ARGS__)
  #define SPLIT_\
  LIMIT 7

  struct Config {
  #define CONFIG_VERSION 3
      int version = CONFIG_VERSION;
  };
snapshot: |
  - { name: "MAX_BUFFER_SIZE", kind: Constant, range: "9:8-9:28", selection_range: "9:8-9:23" }
  - { name: "CHECK", kind: Constant, range: "10:8-10:45", selection_range: "10:8-10:13", detail: "(cond, msg)" }
  - { name: "TRACE", kind: Constant, range: "11:8-11:35", selection_range: "11:8-11:13", detail: "(...)" }
  - { name: "SPLIT_LIMIT", kind: Constant, range: "12:8-13:7", selection_range: "12:8-13:5" }
  - { name: "Config", kind: Struct, range: "15:0-18:1", selection_range: "15:7-15:13", detail: "struct" }
  -   { name: "CONFIG_VERSION", kind: Constant, range: "16:8-16:24", selection_range: "16:8-16:22" }
  -   { name: "version", kind: Field, range: "17:4-17:32", selection_range: "17:8-17:15", detail: "int" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Macros in the preamble region**

Definitions in the leading directive run outline on the inspect path, while the server's preamble record does not surface them yet

```snap-document_symbol
feature: document_symbol
code: |
  #define PREAMBLE_LIMIT 8
  #define PREAMBLE_CHECK(cond) (!!(cond))

  int after = PREAMBLE_LIMIT;
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Detail

<!-- BEGIN GENERATED ITEMS: symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**Function signatures**

Parameter and return types in the `detail` field disambiguate overloads; constructors drop the `void` return type

```snap-document_symbol
feature: document_symbol
code: |
  namespace detail {

  void process(int x);
  void process(const char* s);

  struct Task {
      Task();
      Task(int priority);

      int run(bool async) const;
  };

  }  // namespace detail
snapshot: |
  - { name: "detail", kind: Namespace, range: "5:0-17:1", selection_range: "5:10-5:16" }
  -   { name: "process", kind: Function, range: "7:0-7:19", selection_range: "7:5-7:12", detail: "void (int)" }
  -   { name: "process", kind: Function, range: "8:0-8:27", selection_range: "8:5-8:12", detail: "void (const char *)" }
  -   { name: "Task", kind: Struct, range: "10:0-15:1", selection_range: "10:7-10:11", detail: "struct" }
  -     { name: "Task", kind: Method, range: "11:4-11:10", selection_range: "11:4-11:8", detail: "()" }
  -     { name: "Task", kind: Method, range: "12:4-12:22", selection_range: "12:4-12:8", detail: "(int)" }
  -     { name: "run", kind: Method, range: "14:4-14:29", selection_range: "14:8-14:11", detail: "int (bool) const" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable and field types**

The declared type in the `detail` field; lambdas render as `(lambda)`

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "detail", kind: Namespace, range: "4:0-18:1", selection_range: "4:10-4:16" }
  -   { name: "timeout", kind: Variable, range: "6:0-6:16", selection_range: "6:4-6:11", detail: "int" }
  -   { name: "logger_name", kind: Variable, range: "7:0-7:32", selection_range: "7:12-7:23", detail: "const char *" }
  -   { name: "Config", kind: Struct, range: "9:0-12:1", selection_range: "9:7-9:13", detail: "struct" }
  -     { name: "retries", kind: Field, range: "10:4-10:20", selection_range: "10:13-10:20", detail: "unsigned int" }
  -     { name: "backoff", kind: Field, range: "11:4-11:18", selection_range: "11:11-11:18", detail: "double" }
  -   { name: "on_error", kind: Variable, range: "14:0-16:1", selection_range: "14:5-14:13", detail: "(lambda)" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#221 -->

**Default argument stripping**

The signature is derived from the function type, so default parameter values never leak into the outline

```snap-document_symbol
feature: document_symbol
code: |
  namespace detail {

  void open_file(const char* path, int mode = 0644);

  struct Server {
      void listen(int port = 8080, int backlog = 128);
  };

  }  // namespace detail
snapshot: |
  - { name: "detail", kind: Namespace, range: "5:0-13:1", selection_range: "5:10-5:16" }
  -   { name: "open_file", kind: Function, range: "7:0-7:49", selection_range: "7:5-7:14", detail: "void (const char *, int)" }
  -   { name: "Server", kind: Struct, range: "9:0-11:1", selection_range: "9:7-9:13", detail: "struct" }
  -     { name: "listen", kind: Method, range: "10:4-10:51", selection_range: "10:9-10:15", detail: "void (int, int)" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Base classes in detail**

Show `: Shape` on derived class declarations

```snap-document_symbol
feature: document_symbol
code: |
  struct Shape {};

  struct Circle : Shape {
      double radius;
  };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**Multiline signature ranges**

The symbol range starts at the beginning of the declaration and spans the full signature, so editor sticky scroll anchors correctly

```snap-document_symbol
feature: document_symbol
code: |
  struct Config {};

  void process_data(
      const Config& cfg,
      int flags
  ) {}
snapshot: |
  - { name: "Config", kind: Struct, range: "5:0-5:16", selection_range: "5:7-5:13", detail: "struct" }
  - { name: "process_data", kind: Function, range: "7:0-10:4", selection_range: "7:5-7:17", detail: "void (const Config &, int)" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scoped types**

A written class scope appears in the detail exactly once, for nested classes, template-ids, aliases and dependent names alike

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "scoped", kind: Namespace, range: "4:0-26:1", selection_range: "4:10-4:16" }
  -   { name: "Outer", kind: Struct, range: "6:0-10:1", selection_range: "6:7-6:12", detail: "struct" }
  -     { name: "Inner", kind: Struct, range: "7:4-7:19", selection_range: "7:11-7:16", detail: "struct" }
  -     { name: "Box", kind: Struct, range: "8:26-8:39", selection_range: "8:33-8:36", detail: "template struct" }
  -     { name: "Alias", kind: Class, range: "9:4-9:21", selection_range: "9:10-9:15", detail: "type alias" }
  -   { name: "User", kind: Struct, range: "12:0-17:1", selection_range: "12:7-12:11", detail: "struct" }
  -     { name: "plain", kind: Field, range: "13:4-13:22", selection_range: "13:17-13:22", detail: "Outer::Inner" }
  -     { name: "boxed", kind: Field, range: "14:4-14:25", selection_range: "14:20-14:25", detail: "Outer::Box<int>" }
  -     { name: "aliased", kind: Field, range: "15:4-15:24", selection_range: "15:17-15:24", detail: "Outer::Alias" }
  -     { name: "frozen", kind: Field, range: "16:4-16:29", selection_range: "16:23-16:29", detail: "const Outer::Inner" }
  -   { name: "Holder", kind: Struct, range: "20:0-24:1", selection_range: "20:7-20:13", detail: "template struct" }
  -     { name: "value", kind: Field, range: "21:4-21:26", selection_range: "21:21-21:26", detail: "typename T::type" }
  -     { name: "deep", kind: Field, range: "22:4-22:32", selection_range: "22:28-22:32", detail: "typename T::inner::type" }
  -     { name: "bound", kind: Field, range: "23:4-23:42", selection_range: "23:37-23:42", detail: "typename T::template rebind<int>" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Missing Symbols

<!-- BEGIN GENERATED ITEMS: missing_symbols -->

<!-- BEGIN CAPABILITY: unsupported clangd#2226 -->

**Include directives**

`#include` entries in the outline

```snap-document_symbol
feature: document_symbol
code: |
  #include "config.h"

  int uses_config();
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**Local symbols**

Variables and types declared inside function bodies nest under their function

```snap-document_symbol
feature: document_symbol
code: |
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
snapshot: |
  - { name: "compute", kind: Function, range: "5:0-24:1", selection_range: "5:4-5:11", detail: "int ()" }
  -   { name: "local_sum", kind: Variable, range: "6:4-6:21", selection_range: "6:8-6:17", detail: "int" }
  -   { name: "Accumulator", kind: Struct, range: "8:4-10:5", selection_range: "8:11-8:22", detail: "struct" }
  -     { name: "total", kind: Field, range: "9:8-9:17", selection_range: "9:12-9:17", detail: "int" }
  -   { name: "twice", kind: Variable, range: "12:4-14:5", selection_range: "12:9-12:14", detail: "(lambda)" }
  -   { name: "Pair", kind: Struct, range: "16:4-19:5", selection_range: "16:11-16:15", detail: "struct" }
  -     { name: "a", kind: Field, range: "17:8-17:13", selection_range: "17:12-17:13", detail: "int" }
  -     { name: "b", kind: Field, range: "18:8-18:13", selection_range: "18:12-18:13", detail: "int" }
  -   { name: "first", kind: Variable, range: "21:10-21:15", selection_range: "21:10-21:15", detail: "int" }
  -   { name: "second", kind: Variable, range: "21:17-21:23", selection_range: "21:17-21:23", detail: "int" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module declarations**

`export module`, `module` and `import` declarations in the outline

```snap-document_symbol
feature: document_symbol
code: |
  export module app.core;

  import std;

  export int core_entry();
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`#pragma mark` navigation markers**

Editor section markers as outline entries

```snap-document_symbol
feature: document_symbol
code: |
  #pragma mark - Lifecycle

  void setup();

  #pragma mark - Rendering

  void draw();
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend function definitions**

A friend function defined inline in a class appears under that class

```snap-document_symbol
feature: document_symbol
code: |
  struct Owner {
      friend void inline_friend(Owner& o) {}

      friend bool operator==(const Owner& lhs, const Owner& rhs) {
          return &lhs == &rhs;
      }
  };
snapshot: |
  - { name: "Owner", kind: Struct, range: "4:0-10:1", selection_range: "4:7-4:12", detail: "struct" }
  -   { name: "inline_friend", kind: Function, range: "5:4-5:42", selection_range: "5:16-5:29", detail: "void (Owner &)" }
  -   { name: "operator==", kind: Operator, range: "7:4-9:5", selection_range: "7:16-7:26", detail: "bool (const Owner &, const Owner &)" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Tags

<!-- BEGIN GENERATED ITEMS: symbol_tags -->

<!-- BEGIN CAPABILITY: unsupported -->

**Deprecated tag**

Mark `[[deprecated]]` symbols with the LSP `deprecated` symbol tag

```snap-document_symbol
feature: document_symbol
code: |
  [[deprecated("use open_v2")]] void open_v1();

  void open_v2();
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2123 -->

**Access and storage indicators**

Public / private / protected, static, virtual and abstract markers on outline entries

```snap-document_symbol
feature: document_symbol
code: |
  class Base {
  public:
      virtual void render() = 0;

  protected:
      static int instances();

  private:
      int id;
  };
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Location Correctness

<!-- BEGIN GENERATED ITEMS: location_correctness -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**Symbols from macro expansions**

A symbol produced by a macro invocation is located at the invocation, not at the macro definition

```snap-document_symbol
feature: document_symbol
code: |
  // The assertion holds the directives out of the preamble region, whose
  // live record the server path does not yet see.
  static_assert(true);

  #define DEFINE_HANDLER(name) void name()

  DEFINE_HANDLER(on_ready);
  DEFINE_HANDLER(on_close);

  #define DECLARE_CLASS(X) class X
  DECLARE_CLASS(Generated) {
      int member;
  };
snapshot: |
  - { name: "DEFINE_HANDLER", kind: Constant, range: "9:8-9:40", selection_range: "9:8-9:22", detail: "(name)" }
  - { name: "on_ready", kind: Function, range: "11:0-11:23", selection_range: "11:15-11:23", detail: "void ()" }
  - { name: "on_close", kind: Function, range: "12:0-12:23", selection_range: "12:15-12:23", detail: "void ()" }
  - { name: "DECLARE_CLASS", kind: Constant, range: "14:8-14:32", selection_range: "14:8-14:21", detail: "(X)" }
  - { name: "Generated", kind: Class, range: "15:0-17:1", selection_range: "15:14-15:23", detail: "class" }
  -   { name: "member", kind: Field, range: "16:4-16:14", selection_range: "16:8-16:14", detail: "int" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**Names spelled in macro arguments**

The selection range points at the name written in the macro argument; names spelled in the macro body fall back to the invocation site

```snap-document_symbol
feature: document_symbol
code: |
  // The assertion holds the directives out of the preamble region, whose
  // live record the server path does not yet see.
  static_assert(true);

  #define VAR(X) int X = 1;

  VAR(from_argument)

  #define COUNTER() int counter_from_body = 0;

  COUNTER()
snapshot: |
  - { name: "VAR", kind: Constant, range: "9:8-9:25", selection_range: "9:8-9:11", detail: "(X)" }
  - { name: "from_argument", kind: Variable, range: "11:0-11:17", selection_range: "11:4-11:17", detail: "int" }
  - { name: "COUNTER", kind: Constant, range: "13:8-13:44", selection_range: "13:8-13:15", detail: "()" }
  - { name: "counter_from_body", kind: Variable, range: "15:0-15:7", selection_range: "15:0-15:7", detail: "int" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
