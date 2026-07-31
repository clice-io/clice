# Semantic Tokens

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/feature_docs.ts update`. -->

clice classifies every token of a document with its own token-kind vocabulary,
which is richer than the standard LSP token types and consistent across all
clice replies. Clients that prefer standard LSP kinds can map them through
configuration.

## Lexical Tokens

Kinds derived from the token stream itself, independent of the AST.

<!-- BEGIN GENERATED ITEMS: Lexical Tokens -->

- [x] Comments — line, block and doc comments, including multiline blocks

  ```cpp
  // A line comment.
  /* a one-line block comment */
  /*
   * a block comment
   * spanning several lines
   */
  /// a doc comment
  int after_comments = 0;

  /* first
  second */ int after_block = 1;
  ```

- [x] Literals — numbers, characters and strings, including raw strings

  ```cpp
  int decimal = 42;
  int hexadecimal = 0xFF;
  double floating = 3.14;
  char letter = 'x';
  const char* text = "hello";
  const char* raw = R"(no "escapes" in here)";
  int after_raw = 1;

  const char* multiline = R"(line1
  line2
  )"; int after_closing = 2;
  ```

- [x] Keywords — including alternative operator spellings and the contextual `final` / `override`

  ```cpp
  bool logic(bool a, bool b) {
      return a and b or not a;
  }

  struct Base {
      virtual void act();
      virtual ~Base();
  };

  struct Leaf final : Base {
      void act() override;
  };

  struct Last : Base {
      void act() final;
  };
  ```

- [x] Preprocessor directives — `#if` chains keep directive kinds; disabled branches keep lexical kinds; pragma arguments stay plain

  ```cpp
  int before_conditional = 0;

  #if 0
  int disabled_branch;
  #else
  int enabled_branch = 1;
  #endif

  #define FLAG
  #ifdef FLAG
  int flagged = 2;
  #endif

  #pragma pack(1)
  ```

- [x] Header names — quoted and angled `#include` filenames, including the split `# include` form

  ```cpp
  #include "inc/angled.h"
  #include <angled.h>
  # include "inc/angled.h"

  int after_includes = 0;
  ```

- [ ] Literal prefixes and suffixes — encoding prefixes, type suffixes, digit separators and UDL suffixes as distinct tokens

  ```cpp
  using size_type = decltype(sizeof(0));
  constexpr size_type operator""_kb(unsigned long long n) {
      return n * 1024;
  }

  auto wide = L"wide string";
  auto utf8 = u8"utf-8 string";
  auto hex = 0xFF;
  auto binary = 0b1010;
  auto unsigned_suffix = 42u;
  auto float_suffix = 3.14f;
  auto separators = 1'000'000;
  auto udl = 4_kb;
  ```

- [ ] Escape sequences — highlighted distinctly inside string and character literals

  ```cpp
  const char* escaped = "hello\nworld";
  char hex_escape = '\x41';
  ```

- [ ] Declarator vs operator disambiguation — `*`, `&`, `&&` as declarators vs arithmetic/logical operators ([clangd#1421](https://github.com/clangd/clangd/issues/1421))

  ```cpp
  int value = 1;
  int* pointer = &value;
  int& reference = value;
  int product = value * value;
  int masked = value & 1;
  ```

- [ ] Primitive token type — a distinct kind for built-in types instead of plain `keyword`

  ```cpp
  int number = 0;
  float ratio = 0.5f;
  void act();
  ```

- [ ] Bracket token types — matching `()`, `[]`, `{}`, `<>` pairs as distinct kinds

  ```cpp
  template <typename T>
  struct Grid {
      T cells[4];
  };

  Grid<int> grid{{1, 2, 3, 4}};

  int first(Grid<int>& grid) {
      return grid.cells[0];
  }
  ```

<!-- END GENERATED ITEMS -->

## Declarations & References

Names classified by the declaration they define or reference.

<!-- BEGIN GENERATED ITEMS: Declarations & References -->

- [x] Namespaces — definitions, references, nested namespaces and namespace aliases

  ```cpp
  namespace demo {
  namespace inner {
  int value = 1;
  }
  }

  namespace demo::inner::more {}

  namespace alias = demo::inner;

  int use_alias = alias::value;
  ```

- [x] Types — class, struct, union, enum and type aliases, at definitions and references

  ```cpp
  class Widget {};
  struct Point {};
  union Storage {
      int i;
      float f;
  };
  enum Flags { FlagA };
  enum class Mode { Fast };

  typedef Point PointAlias;
  using WidgetAlias = Widget;

  Widget* make_widget();
  PointAlias origin;
  Mode current = Mode::Fast;
  ```

- [x] Functions and methods — declarations, definitions and call sites

  ```cpp
  int twice(int value);

  int twice(int value) {
      return value * 2;
  }

  struct Machine {
      void start();
      static void reset();
  };

  void drive(Machine machine) {
      machine.start();
      Machine::reset();
      int four = twice(2);
  }
  ```

- [x] Variables — globals, locals, parameters, fields and enum members

  ```cpp
  struct Holder {
      int field;
      static int shared;
  };

  enum class State { Idle };

  int global_value = 1;

  void touch(int param) {
      int local = param + global_value;
      Holder h;
      h.field = local;
      Holder::shared = h.field;
      State state = State::Idle;
  }
  ```

- [x] Templates — type and non-type template parameters, with the `templated` modifier on template names

  ```cpp
  template <typename T, int N>
  struct Array {
      T data[N];
  };

  template <typename T>
  T identity(T value);

  template <typename T>
  T identity(T value) {
      return value;
  }

  Array<int, 4> arr;
  int result = identity(3);
  ```

- [x] Concepts — definitions and uses as template constraints

  ```cpp
  template <typename T>
  concept Small = sizeof(T) <= 4;

  template <Small T>
  void use_small(T value);

  template <typename T>
      requires Small<T>
  void require_small(T value);
  ```

- [x] Labels — `goto` targets and label definitions

  ```cpp
  void retry(bool again) {
      goto done;
  done:
      if (again) {
          goto done;
      }
  }
  ```

- [x] Structured bindings — binding names at definition and use

  The opening `[` deliberately carries no token; only the binding names
  themselves are highlighted.

  ```cpp
  struct Pair {
      int first, second;
  };

  void unpack() {
      auto [a, b] = Pair{1, 2};
      int sum = a + b;
  }
  ```

- [x] Member initializer lists — initialized fields highlighted as fields ([clangd#122](https://github.com/clangd/clangd/issues/122))

  ```cpp
  struct Widget {
      int width;
      int height;

      Widget(int w, int h) : width(w), height(h) {}
  };
  ```

- [x] Using declarations — the introduced name keeps its target's kind ([clangd#2619](https://github.com/clangd/clangd/issues/2619))

  ```cpp
  namespace tools {
  inline int helper() {
      return 1;
  }
  struct Gadget {};
  }

  using tools::helper;
  using tools::Gadget;

  int used = helper();
  Gadget gadget;
  ```

- [x] Lambda init-captures — the captured name highlighted as a variable ([clangd#868](https://github.com/clangd/clangd/issues/868))

  ```cpp
  int compute();

  auto fn = [val = compute()] {
      return val;
  };
  ```

- [x] `sizeof...` — the pack parameter keeps its type-parameter token ([clangd#213](https://github.com/clangd/clangd/issues/213))

  ```cpp
  template <typename... Ts>
  constexpr auto count = sizeof...(Ts);
  ```

- [x] `using enum` — the enum name highlighted at the using site ([clangd#1283](https://github.com/clangd/clangd/issues/1283))

  ```cpp
  enum class Color { Red };

  void paint() {
      using enum Color;
      auto c = Red;
  }
  ```

- [x] Deduction guides — the guide name and the guided template highlighted

  ```cpp
  template <typename T>
  struct Vec {
      template <typename It>
      Vec(It first, It last);
  };

  template <typename It>
  Vec(It, It) -> Vec<int>;
  ```

- [x] Explicit instantiation — the instantiated template name highlighted ([clangd#316](https://github.com/clangd/clangd/issues/316))

  ```cpp
  template <typename T>
  struct Holder {
      T value;
  };

  template struct Holder<int>;
  ```

- [ ] Dependent names — resolved through the primary template where one is known _(partial)_ ([clangd#154](https://github.com/clangd/clangd/issues/154), [clangd#297](https://github.com/clangd/clangd/issues/297))

  Dependent members of a known template (`Box<T>`) resolve to the primary
  template's declarations and keep their kinds. Members of a bare template
  parameter have no candidate declaration and currently get no token;
  heuristic coloring for such names remains open.

  ```cpp
  template <typename T>
  struct Box {
      using value_type = int;
      static void reset();
      int size() const;
  };

  template <typename T>
  void resolved(Box<T> box) {
      typename Box<T>::value_type item;
      Box<T>::reset();
      box.size();
  }

  template <typename T>
  void unresolved(T value) {
      typename T::value_type item;
      T::reset();
      value.size();
  }
  ```

- [x] Variable templates — declarations, definitions, partial and full specializations

  ```cpp
  template <typename T, typename U>
  extern int pair_value;

  template <typename T, typename U>
  int pair_value = 2;

  template <typename T>
  extern int pair_value<T, int>;

  template <typename T>
  int pair_value<T, int> = 4;

  template <>
  int pair_value<int, int> = 5;
  ```

- [x] Out-of-line member definitions — qualified names keep method kinds and modifiers

  ```cpp
  struct Gauge {
      int read() const;
      static void reset();
  };

  int Gauge::read() const {
      return 0;
  }

  void Gauge::reset() {}
  ```

- [x] Alias templates — the alias name carries the type kind and the `templated` modifier

  ```cpp
  template <typename T>
  using Ptr = T*;

  template <typename T>
  struct Box {};

  template <typename T>
  using BoxPtr = Box<T>*;

  Ptr<int> pointer = nullptr;
  ```

- [x] Template template parameters — declared and used as types

  ```cpp
  template <typename T>
  struct Holder {};

  template <template <typename> class Container, typename T>
  struct Adaptor {
      Container<T> value;
  };

  Adaptor<Holder, int> adaptor;
  ```

- [x] Lambda captures — by-copy and by-reference captures reference the captured variable; `this` stays a keyword

  ```cpp
  struct S {
      int field;

      int compute() {
          int local = 1;
          auto by_copy = [local, this] {
              return local + this->field;
          };
          auto by_reference = [&local] {
              return local;
          };
          return by_copy() + by_reference();
      }
  };
  ```

- [x] Range-based for — the loop variable at definition and use

  ```cpp
  struct List {
      int* begin();
      int* end();
  };

  void iterate(List items) {
      for (auto& item : items) {
          item = 0;
      }
  }
  ```

- [x] Enum underlying types — the enum-base reference keeps its type kind

  ```cpp
  using Byte = unsigned char;

  enum class Flags : Byte { A, B };

  Flags flags = Flags::A;
  ```

- [x] Friend declarations — befriended names resolve to their targets; inline friends define

  ```cpp
  struct Widget;
  void ping();

  struct Host {
      friend struct Widget;
      friend void ping();
      friend void inline_friend() {}
  };
  ```

- [ ] Dependent using declarations — `using T::name` in a template body _(partial)_

  The introduced name and its uses currently get no token; the reserved
  dependent-name modifier is not emitted yet.

  ```cpp
  template <typename T>
  struct Derived : T {
      using T::value;

      int use() {
          return value;
      }
  };
  ```

<!-- END GENERATED ITEMS -->

## Modules

<!-- BEGIN GENERATED ITEMS: Modules -->

- [x] Module declarations — the contextual `module` keyword, dotted module names and the private fragment

  ```cpp
  module;

  export module demo.core;

  export int exported_value = 1;

  module :private;

  int private_value = 2;

  #if 0
  module :private;
  #endif
  ```

- [x] Module partitions — partition names in the module declaration

  ```cpp
  export module demo.core:part;

  export int partition_value = 1;
  ```

- [x] `module` and `import` as identifiers — contextual keywords keep their semantic kinds outside module declarations

  ```cpp
  void f() {
      struct module {};
      module m;
      int import = 1;
      int module = 2;
  }
  ```

<!-- END GENERATED ITEMS -->

## Token Modifiers

<!-- BEGIN GENERATED ITEMS: Token Modifiers -->

- [x] Declaration vs definition — the modifier distinguishes the two

  ```cpp
  int measure(int value);

  int measure(int value) {
      return value;
  }

  struct Sensor;

  struct Sensor {};
  ```

- [x] Static — class-level members and static locals

  ```cpp
  struct Counter {
      static int total;
      static void bump();
      int current;
  };

  void count() {
      static int calls = 0;
      Counter::bump();
      Counter::total = calls;
  }
  ```

- [x] Readonly — const and constexpr values, const methods and enum members

  Readonly is currently value-based: a pointer to const counts as
  readonly even though the pointer itself can change.

  ```cpp
  enum class Level { High };

  const int limit = 10;
  constexpr int bound = 4;

  struct Gauge {
      int read() const;
      void write(int value);
  };

  void probe(const int& in, const int* pointee_const, int* const self_const) {
      Gauge gauge;
      gauge.read();
      gauge.write(limit);
  }
  ```

- [x] Virtual and abstract — virtual methods, pure virtual methods and abstract classes

  ```cpp
  struct Shape {
      virtual int area();
      virtual int perimeter() = 0;
      virtual ~Shape();
  };

  struct Square : Shape {
      int perimeter() override;
  };

  int measure(Shape& shape) {
      return shape.area() + shape.perimeter();
  }
  ```

- [x] Deprecated — `[[deprecated]]` declarations and their uses

  ```cpp
  [[deprecated("use next_api")]] void old_api();
  void next_api();

  void migrate() {
      old_api();
  }
  ```

- [x] Default library — symbols declared in system headers

  ```cpp
  int before_includes = 0;

  #include <syslib.h>

  int used = system_helper();
  ```

- [ ] Scope modifiers — function, class, file and global scope ([clangd#352](https://github.com/clangd/clangd/issues/352))

  ```cpp
  int global_scope;
  static int file_scope;

  struct Foo {
      int class_scope;

      void bar() {
          int function_scope = 0;
      }
  };
  ```

- [ ] Mutable reference and pointer — arguments passed by non-const reference or pointer ([clangd#839](https://github.com/clangd/clangd/issues/839))

  ```cpp
  void modify(int& out);
  void modify_through(int* out);
  void inspect(const int& in);

  void run() {
      int value = 0;
      modify(value);
      modify_through(&value);
      inspect(value);
  }
  ```

- [ ] Deduced — mark deduced types such as `auto` and `decltype`

  ```cpp
  auto deduced_int = 1;
  decltype(deduced_int) same_type = 2;
  ```

- [ ] User-defined operators — distinguish overloaded operators from built-in ones ([clangd#1521](https://github.com/clangd/clangd/issues/1521))

  ```cpp
  struct Vec {
      Vec operator+(const Vec& other) const;
  };

  Vec add(Vec a, Vec b) {
      return a + b;
  }

  int add(int a, int b) {
      return a + b;
  }
  ```

<!-- END GENERATED ITEMS -->

## Conflict & Ambiguity

C++ allows structurally different entities to share one name. When a single
written name refers to entities of different kinds at once, no single token
type is correct; such names receive the dedicated **conflict** token type,
which clients typically display in a neutral color.

<!-- BEGIN GENERATED ITEMS: Conflict & Ambiguity -->

- [x] Type vs function — a name naming both renders as `conflict`

  ```cpp
  namespace shop {
  struct Widget {};
  void Widget();
  }

  using shop::Widget;
  ```

- [x] Type vs variable — a name naming both renders as `conflict`

  ```cpp
  namespace mixed {
  struct Thing {};
  int Thing;
  }

  using mixed::Thing;
  ```

- [x] Same-kind overload sets — a name naming only functions is no conflict

  ```cpp
  namespace ops {
  void apply();
  void apply(int level);
  }

  using ops::apply;

  void run() {
      apply();
      apply(1);
  }
  ```

- [x] Injected class name — the class name used as a constructor call inside the class

  The written name renders as the class; the constructor reference it
  implies paints nothing extra — the `(` stays token-free.

  ```cpp
  struct Widget {
      Widget(int size);

      Widget create() {
          return Widget(42);
      }
  };
  ```

<!-- END GENERATED ITEMS -->

## Token Correctness

Shapes clice pins deliberately, including issues clangd got wrong.

<!-- BEGIN GENERATED ITEMS: Token Correctness -->

- [x] Constructors and destructors — method tokens with the constructor/destructor modifier ([clangd#1509](https://github.com/clangd/clangd/issues/1509), [clangd#2078](https://github.com/clangd/clangd/issues/2078), [clangd#872](https://github.com/clangd/clangd/issues/872))

  A destructor name renders as two tokens: the `~` carries the method
  kind and the declaration/definition modifiers, the class name after it
  stays a reference to the class.

  ```cpp
  struct Session {
      Session();
      ~Session();
  };

  Session::Session() {}

  Session::~Session() {}

  void destroy(Session* session) {
      session->~Session();
  }
  ```

- [x] Anonymous parameters — unnamed parameters produce no tokens

  The punctuation after an unnamed parameter's type stays token-free.

  ```cpp
  void take_one(int) {}
  void take_two(int, char* c) {}
  ```

- [x] Operator names — the `operator` keyword and call-site punctuation stay plain

  An operator's written name is keyword plus punctuation, so no name
  token is painted: `operator` keeps its keyword classification and
  call sites emit nothing on the operator symbol.

  ```cpp
  struct Value {
      Value& operator=(const Value& other);
      Value operator+(const Value& other) const;
  };

  void combine(Value a, Value b) {
      a = b;
      Value c = a + b;
  }
  ```

- [x] Destructors of class templates — the `~` shape holds under templates

  ```cpp
  template <typename T>
  struct Holder {
      ~Holder();
  };

  template <typename T>
  Holder<T>::~Holder() {}
  ```

- [x] Conversion operators — written as keywords, converting uses paint nothing extra

  ```cpp
  struct Ratio {
      operator double() const;
      explicit operator bool() const;
  };

  double to_double(Ratio ratio) {
      if (ratio) {
          return ratio;
      }
      return double(ratio);
  }
  ```

- [x] Pseudo-destructor on a template parameter — the `~` paints nothing; the type name keeps its kind

  ```cpp
  template <typename T>
  void reset(T* value) {
      value->~T();
  }
  ```

- [x] Defaulted and deleted members — special-member names keep their definition tokens

  ```cpp
  struct Session {
      Session() = default;
      Session(const Session&) = delete;
      ~Session() = default;
  };
  ```

<!-- END GENERATED ITEMS -->

## Attributes

<!-- BEGIN GENERATED ITEMS: Attributes -->

- [ ] Attribute names — standard and vendor attributes, and expressions inside them ([clangd#2209](https://github.com/clangd/clangd/issues/2209))

  ```cpp
  [[nodiscard]] int compute();
  [[deprecated("use v2")]] void old_func();
  [[maybe_unused]] int counter = 0;

  struct [[gnu::packed]] Packed {};
  ```

<!-- END GENERATED ITEMS -->

## Macros

Tokens inside macro definition bodies keep their lexical kinds; highlighting
them from their expansions belongs to a future expansion-preview feature.

<!-- BEGIN GENERATED ITEMS: Macros -->

- [x] Macro definition and expansion

  ```cpp
  #define SQUARE(x) ((x) * (x))

  [[maybe_unused]] static int squared = SQUARE(4);
  ```

- [x] Expansion sites and arguments — expansion names are macros, written arguments keep their semantics, definition bodies stay lexical

  ```cpp
  int value = 1;

  #define ID(x) x
  #define CALL helper()

  void helper();

  int copied = ID(value);

  void run() {
      CALL;
  }
  ```

- [ ] Object-like vs function-like macros — distinct highlighting for the two forms ([clangd#2649](https://github.com/clangd/clangd/issues/2649))

  ```cpp
  #define MAX_SIZE 1024
  #define CHECK(x) ((x) ? 1 : 0)

  int checked = CHECK(MAX_SIZE);
  ```

<!-- END GENERATED ITEMS -->

## Other Known Gaps

Curated issues without a fixture yet:

- `auto` parameters must not be highlighted as template type parameters
  ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- Nested name specifier in a pointer-to-member should get a token
  ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- `::new` should keep the `new` keyword highlighted
  ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- `co_yield` / `co_await` lose highlighting when the coroutine return type is
  a template ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- Token modifiers should apply to operands of overloaded operators
  ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- Dependent template names (`obj.template get<int>()`), members imported from
  a dependent base via `using`, and dependent names with mixed-kind overload
  sets ([clangd#484](https://github.com/clangd/clangd/issues/484),
  [clangd#686](https://github.com/clangd/clangd/issues/686),
  [clangd#1057](https://github.com/clangd/clangd/issues/1057))

## Inactive Code Regions

Inactive preprocessor branches are reported through a separate channel, not
as semantic tokens.

- [ ] Dim inactive preprocessor branches ([clangd#132](https://github.com/clangd/clangd/issues/132))
- [ ] Correct inactive boundaries with `#elif` chains ([clangd#602](https://github.com/clangd/clangd/issues/602))
- [ ] Preserve syntax highlighting within inactive regions ([clangd#1664](https://github.com/clangd/clangd/issues/1664))
- [ ] Keep inactive regions distinct from comments ([clangd#1545](https://github.com/clangd/clangd/issues/1545))
- [ ] Unreachable code dimming ([clangd#1828](https://github.com/clangd/clangd/issues/1828))

## Format String Highlighting

- [ ] `std::format` / `std::print` placeholder highlighting ([clangd#1709](https://github.com/clangd/clangd/issues/1709))
- [ ] Highlight invalid format specifiers as errors

## Protocol Support

- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [x] UTF-16 delta-encoded token positions
- [ ] Range-based semantic tokens (`textDocument/semanticTokens/range`) — only
      compute tokens for the visible viewport, critical for large files
- [ ] Delta updates (`textDocument/semanticTokens/full/delta`) — send only
      changes since the previous response

## Changelog

| Date | Change                                                           | PR  |
| ---- | ---------------------------------------------------------------- | --- |
| —    | Initial semantic token types and modifiers, full document tokens | —   |
