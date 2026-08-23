# Hover

Rich information cards for the symbol under the cursor.

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/feature_docs.ts update`. -->

## Symbol Information

<!-- BEGIN GENERATED ITEMS: Symbol Information -->

- [x] Qualified name — the hover card shows the enclosing namespace and class scope

  <details>
  <summary>Example</summary>

  ```cpp
  namespace app::detail {

  struct Engine {
      void tick() {
          int count = 0;
      }
  };

  int workers = 4;

  }

  int global = 1;
  ```

  </details>

- [x] Symbol kind — the card names what the symbol is: struct, enum, function, field, …

  <details>
  <summary>Example</summary>

  ```cpp
  namespace kinds {

  struct Point {
      int x;
  };

  union Packet {
      int raw;
  };

  enum class Color {
      Red,
  };

  using Alias = Point;

  int length(Point p) {
      return p.x;
  }

  }
  ```

  </details>

- [x] Access specifier — members show their public / protected / private access

  <details>
  <summary>Example</summary>

  ```cpp
  class Account {
  public:
      int balance;

  protected:
      int limit;

  private:
      int pin;
  };
  ```

  </details>

- [x] Definition rendering — the card includes the symbol's source definition

  <details>
  <summary>Example</summary>

  ```cpp
  namespace retry {

  constexpr int max_retries = 3;

  int backoff(int attempt = 1) {
      return attempt * max_retries;
  }

  }
  ```

  </details>

- [ ] Initializer truncation — huge initializers render truncated, not in full _(partial)_ ([clangd#710](https://github.com/clangd/clangd/issues/710))

  The rendered definition omits the initializer, but the evaluated
  `Value` field still spells out all 256 elements.

  <details>
  <summary>Example</summary>

  ```cpp
  #define A(x) x, x, x, x
  #define B(x) A(A(A(A(x))))
  int arr[] = {B(0)};
  ```

  </details>

- [ ] Virtual modifiers — `virtual` / `override` / `final` show on method hover _(partial)_ ([clangd#2474](https://github.com/clangd/clangd/issues/2474))

  Modifiers written in the source render (`virtual … = 0`, `override`,
  `final`), but an overriding method that omits the redundant `virtual`
  keyword gives no sign of its virtuality — the card lacks the
  `virtual void draw() override` form the issue asks for.

  <details>
  <summary>Example</summary>

  ```cpp
  struct Base {
      virtual void draw() = 0;
  };

  struct Circle : Base {
      void draw() override;
  };

  struct Dot final : Circle {
      void draw() final;
  };
  ```

  </details>

- [ ] Anonymous namespace scope — `(anonymous namespace)` shows in the scope display _(partial)_ ([clangd#436](https://github.com/clangd/clangd/issues/436))

  The cards render, but the anonymous segment is dropped from the
  scope display: a top-level anonymous member shows no scope line at
  all, and `outer::(anonymous)` shows just `outer`.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace {
  int hidden = 1;
  }

  namespace outer {
  namespace {
  int nested = 2;
  }
  }

  int sum = hidden + outer::nested;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Type Information

<!-- BEGIN GENERATED ITEMS: Type Information -->

- [x] Variable types — pointers, references, arrays

  A variable's card pretty-prints its declared type, spelling the pointer,
  reference and array declarators the way they read in source.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace variable_type {

  int target;

  int *ptr = &target;

  int &ref = target;

  int numbers[4]{};

  }
  ```

  </details>

- [x] Type aliases — the desugared `aka` form

  A sugared type shows its underlying type as `Alias (aka int)`. The
  `show_aka` option turns the `aka` suffix off.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace aka_desugar {

  using Handle = int;
  using Alias = Handle;

  Handle direct = 0;

  Alias chained = 0;

  }
  ```

  </details>

- [x] Function signatures — return type, parameter names, defaults

  A function's card lists its return type, each parameter with its name,
  and any default argument.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace function_signature {

  int add(int lhs, int rhs);

  void configure(int width, bool visible = true);

  }
  ```

  </details>

- [x] Template parameters — type, template-template, non-type

  Each template parameter kind reports its form: a type parameter, a
  template-template parameter, and a non-type parameter with its default.

  <details>
  <summary>Example</summary>

  ```cpp
  // Template type parameter.
  namespace type_param {
  template <typename T = int> void foo();
  }

  // Template template parameter.
  namespace template_template_param {
  template <template<typename> class T> void foo();
  }

  // Non-type template parameter.
  namespace non_type_param {
  template <int T = 5> void foo();
  }
  ```

  </details>

- [x] `auto` deduction — the type the placeholder resolves to

  Hovering an `auto` placeholder shows the type substituted for it —
  builtins, pointers, lambdas, template instantiations, and the
  `/* not deduced */` marker inside an uninstantiated template.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace auto_deduction {

  struct Bar {};
  struct Pair { int first; int second; };
  template <typename T> struct Box {};

  void locals() {
    int n = 0;
    auto a = 1;
    const auto b = 1;
    auto& c = n;
    auto* d = &n;
    auto e = &n;
    auto f = []{};
    auto g = Box<int>();
    auto [x, y] = Pair{};
  }

  auto with_trailing() -> int { return 0; }

  auto deduced_return() { return Bar(); }

  template <typename T> void undeduced() {
    auto u = T();
  }

  }
  ```

  </details>

- [x] `decltype` deduction — value, reference and dependent forms

  Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
  type, including the reference the parenthesized-expression rule adds.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace decltype_deduction {

  int base = 0;

  void locals() {
    int n = 0;
    const int cn = 0;
    int& r = n;
    decltype(auto) a = 1;
    decltype(auto) b = cn;
    decltype(auto) c = r;
    decltype(n) d = n;
    decltype((n)) e = n;
    decltype(static_cast<int&&>(n)) f = static_cast<int&&>(n);
  }

  decltype(base) mirror = base;

  template <typename T> decltype(auto) undeduced() { return T(); }

  template <typename T> struct Dependent {
    using kind = decltype(T::member);
  };

  }
  ```

  </details>

- [ ] CTAD — deduced template arguments of a class placeholder _(partial)_ ([clangd#435](https://github.com/clangd/clangd/issues/435))

  With class template argument deduction the variable's card shows the
  deduced `Box<int>`, but hovering the class-name spelling still reports
  the primary template without its arguments.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace ctad_arguments {

  template <typename T> struct Box {
    Box(T);
  };

  Box picked(42);

  }
  ```

  </details>

- [ ] Instantiation arguments — template parameters bound at a use site _(partial)_ ([clangd#230](https://github.com/clangd/clangd/issues/230))

  A use of a template shows the substituted types (`Wrapper<int>`,
  `identity<int>`, `int x`), but not an explicit `T = int` mapping of each
  parameter to the argument it was bound to.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace instantiation_args {

  template <typename T> struct Wrapper {
    T value;
  };

  template <typename T> T identity(T x) {
    return x;
  }

  void demo() {
    Wrapper<int> holder;
    int r = identity(42);
  }

  }
  ```

  </details>

- [ ] Lambda `auto` parameters — deduced parameter type ([clangd#493](https://github.com/clangd/clangd/issues/493))

  Hovering the `auto` parameter of a generic lambda yields no card; the
  deduced parameter type is not shown.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace lambda_auto_params {

  auto printer = [](auto value) { return value; };

  }
  ```

  </details>

- [x] Sugared `auto` — alias sugar preserved through deduction

  clangd tracks lost alias sugar through `auto` as clangd#709; clice
  already keeps the alias spelling and appends its desugared form, so
  `auto` deduced from an aliased return type reads as `Outer // aka: int`.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace sugared_auto {

  using Inner = int;
  using Outer = Inner;

  Outer make();

  void demo() {
    auto value = make();
  }

  }
  ```

  </details>

- [ ] Type formatting — clang-format applied to rendered types ([clangd#2156](https://github.com/clangd/clangd/issues/2156))

  Long or nested types are printed by the compiler's default type printer;
  they are not re-wrapped or aligned through clang-format.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace clang_format_types {

  template <typename A, typename B, typename C, typename D>
  struct Tuple {};

  Tuple<int, long, unsigned, char> wide;

  }
  ```

  </details>

- [x] Anonymous struct typedef — the classic C `typedef struct {…} Name` ([clangd#2219](https://github.com/clangd/clangd/issues/2219))

  Compiled as C11: clangd renders a misleading `struct Point` for the
  alias of an anonymous struct; clice names the struct after its typedef,
  so both the alias and a variable of it report a clean `Point` card.

  <details>
  <summary>Example</summary>

  ```cpp
  /// A 2-D point.
  typedef struct {
    int x, y;
  } Point;

  Point origin = {.y = 2, .x = 1};
  ```

  </details>

- [ ] Concept constraints — the constraint behind a parameter or `auto` placeholder _(partial)_

  The constrained-parameter and concept-reference cards carry the
  constraint, but hovering the placeholder of a constrained `Addable auto`
  variable shows only the deduced type — the constraint is dropped.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace concept_constraints {

  template <typename T>
  concept Addable = requires(T a) { a + a; };

  template <Addable U>
  void sum(U a, U b);

  auto flag = Addable<int>;

  Addable auto total = 1;

  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Layout Information

<!-- BEGIN GENERATED ITEMS: Layout Information -->

- [x] Field layout — size, offset, alignment and padding show on field hover

  The corpus pins an x86-64 target, so the bit numbers are stable.

  <details>
  <summary>Example</summary>

  ```cpp
  struct Header {
      char tag;
      int length;
  };

  struct Flags {
      int ready : 1;
      int end : 1;
  };
  ```

  </details>

- [ ] Type-level layout — hovering the type itself shows its size, alignment and padding _(partial)_ ([clangd#1763](https://github.com/clangd/clangd/issues/1763))

  Size and alignment show on the type card today; the total padding
  does not yet.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace layout {

  struct Widget {
      int id;
      double value;
  };

  }
  ```

  </details>

- [ ] Vtable offset — virtual methods show their table slot _(partial)_ ([clangd#1771](https://github.com/clangd/clangd/issues/1771))

  The method card renders without any vtable fact today.

  <details>
  <summary>Example</summary>

  ```cpp
  struct Shape {
      virtual void draw();
      virtual void move();
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Expression Context

<!-- BEGIN GENERATED ITEMS: Expression Context -->

- [x] Constant evaluation — constexpr, enumerators, sizeof

  When an initializer is a constant expression, the card evaluates it and
  shows the resulting value.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace constant_value {

  constexpr int square(int n) { return n * n; }
  int from_call = square(5);

  int from_sizeof = sizeof(int);

  enum Color { Red = -1, Green = 5 };
  Color picked = Green;

  template <int A, int B> struct Sum { static constexpr int value = A + B; };
  int from_member = Sum<3, 4>::value;

  }
  ```

  </details>

- [x] Call arguments — which parameter each argument binds to

  Hovering an argument at a call site shows the parameter it is passed to,
  naming the parameter it binds.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace callee_arguments {

  void configure(int width, int& out, int flags = 0);

  void demo() {
    int w = 1024;
    int result = 0;
    configure(w, result, 3);
  }

  }
  ```

  </details>

- [x] Pass semantics — by value, by reference, by const reference

  The argument card states how the value reaches the callee: copied by
  value, or bound to a mutable or const reference parameter.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace pass_semantics {

  void by_value(int x);
  void by_ref(int& x);
  void by_const_ref(const int& x);

  void demo() {
    int n = 0;
    by_value(n);
    by_ref(n);
    by_const_ref(n);
  }

  }
  ```

  </details>

- [x] Implicit conversions — argument converted to the parameter type

  When an argument reaches a parameter through an implicit conversion, the
  card notes the target type, for both built-in and user-defined
  conversions.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace implicit_conversion {

  struct Wrapper {
    Wrapper(int value);
  };

  void take_float(float x);
  void take_wrapper(Wrapper w);

  void demo() {
    int n = 0;
    take_float(n);
    take_wrapper(n);
  }

  }
  ```

  </details>

- [ ] String literals — the length reported on hover _(partial)_ ([clangd#1016](https://github.com/clangd/clangd/issues/1016))

  A string-literal card reports the array type and its size in bytes
  (`const char[6]`, `Size: 6 bytes` — the length plus the null
  terminator), not an explicit character count.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace string_length {

  const char *greeting = "hello";

  }
  ```

  </details>

- [ ] Numeric literals — type and value of an integer or float literal ([clangd#1669](https://github.com/clangd/clangd/issues/1669))

  Hovering a numeric literal yields no card, unlike character and string
  literals, whose type and value are shown.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace numeric_literal_type {

  auto count = 42;
  auto ratio = 3.14;

  }
  ```

  </details>

- [ ] Record variables — enclosing constant value leaks in _(partial)_ ([clangd#1622](https://github.com/clangd/clangd/issues/1622))

  Hovering a record-typed argument of a constant-evaluable call currently
  reports that call's value (`Value = 7`) on the variable — a value that
  is not the record's own.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace record_value_misleading {

  struct Tag {};

  constexpr int rank(Tag) {
    return 7;
  }

  void demo() {
    Tag t;
    int r = rank(t);
  }

  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Documentation

<!-- BEGIN GENERATED ITEMS: Documentation -->

- [x] Doxygen `///` comments — extracted from the declaration and rendered on hover

  Applies to plain functions, primary templates and their specializations;
  a reference resolves to the most specialized declaration's comment.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace docs {
  /// Adds two integers.
  int add(int a, int b);

  /// A box holding a value.
  template <typename T> struct Box {};

  /// A box of pointers.
  template <typename T> struct Box<T*> {};

  void use() {
      Box<int> b;
      Box<int*> p;
  }
  }
  ```

  </details>

- [x] Synthesized accessor docs — trivial getters/setters get a generated one-line description

  A trivial getter or setter with no comment of its own gets a synthesized
  "Trivial accessor/setter for `field`." line in its hover card.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace accessors {
  struct Widget {
      int width;
      int getWidth() { return width; }
      void setWidth(int w) { width = w; }
  };
  }
  ```

  </details>

- [ ] `@copydoc` tags — copy another symbol's documentation onto this one _(partial)_ ([clangd#1320](https://github.com/clangd/clangd/issues/1320))

  A `@copydoc target` tag should copy `target`'s documentation into this
  symbol's hover card. clice does not resolve the tag yet — the card shows
  the literal `@copydoc base_func()` text.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace copydoc {
  /// Detailed documentation.
  void base_func();

  /// @copydoc base_func()
  void wrapper();
  }
  ```

  </details>

- [ ] Inherited override docs — an override with no comment shows the base method's documentation _(partial)_ ([clangd#2504](https://github.com/clangd/clangd/issues/2504))

  Hovering an overriding method that carries no comment of its own should
  surface the documentation from the method it overrides. clice does not
  inherit it yet — the override's card carries no description.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace inherit_docs {
  struct Base {
      /// Renders the widget.
      virtual void draw();
  };
  struct Circle : Base {
      void draw() override;
  };
  }
  ```

  </details>

- [ ] Overload doc sharing — a later overload with no comment reuses the first overload's documentation _(partial)_ ([clangd#2506](https://github.com/clangd/clangd/issues/2506))

  Consecutive overloads often document only the first; a later undocumented
  overload should reuse that shared description. clice does not share it
  yet — the later overload's card carries no description.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace overloads {
  /// Opens a file.
  void open(const char* path);
  void open(const char* path, int flags);
  }
  ```

  </details>

- [ ] Inherited constructor docs — `using Base::Base;` surfaces the base constructor's documentation ([clangd#1936](https://github.com/clangd/clangd/issues/1936))

  A constructor pulled in with `using Base::Base;` should carry the base
  constructor's documentation on hover. There is no hover surface for it:
  the name in the using-declaration resolves to the class, not the
  inherited constructor.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace inherited_ctor {
  struct Base {
      /// Constructs from a value.
      Base(int value);
  };
  struct Derived : Base {
      using Base::Base;
  };
  }
  ```

  </details>

- [ ] Banner comments — a section banner separated by a blank line must not attach to the next declaration _(partial)_ ([clangd#974](https://github.com/clangd/clangd/issues/974))

  A `// ==== Section ====` banner followed by a blank line should not be
  misattributed as documentation for the declaration below it. clice
  currently attaches it anyway — the banner text appears in the card.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace banners {
  // ==== Section Banner ====

  void foo();
  }
  ```

  </details>

- [x] Declaration vs definition comments — the declaration's doc wins over a definition-site comment

  clangd tracks this as clangd#829; clice already prefers the
  declaration's `///` documentation over the definition's plain `//` note,
  showing it at both the declaration and the definition site.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace decldef {
  /// Public API documentation.
  void process(int x);

  // Internal implementation note.
  void process(int x) { (void)x; }
  }
  ```

  </details>

- [ ] Whitespace and newlines — a markdown table in a comment keeps its line breaks _(partial)_ ([clangd#2057](https://github.com/clangd/clangd/issues/2057))

  A markdown table written across several `///` lines should render as a
  table with its line breaks preserved. clice currently flattens the lines
  onto one line, so the table does not render.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace tables {
  /// | Column A | Column B |
  /// |----------|----------|
  /// | 1        | 2        |
  void table_fn();
  }
  ```

  </details>

- [ ] Comment indentation — indented lines in a comment render without spurious extra indentation _(partial)_ ([clangd#1040](https://github.com/clangd/clangd/issues/1040))

  A doc comment whose body contains an indented block should render with
  correct indentation. clice currently strips the leading indentation, so
  an indented code block loses its offset and the blank line collapses.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace indented {
  /// Summary line.
  ///
  ///     step_one();
  ///     step_two();
  void run();
  }
  ```

  </details>

- [ ] Template keyword from a macro — the docstring should survive the expansion _(partial)_ ([clangd#1226](https://github.com/clangd/clangd/issues/1226))

  When the `template` keyword is produced by a macro expansion, the
  declaration's doc comment should still appear on hover. clice currently
  drops it — the card carries no description.

  <details>
  <summary>Example</summary>

  ```cpp
  int anchor = 0;

  #define TEMPLATE template

  /// A documented template function.
  TEMPLATE <typename T> void run(T value);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Macro Hover

<!-- BEGIN GENERATED ITEMS: Macro Hover -->

- [x] Definition text at every site — `#define`, use, `#ifdef` and `#undef` all show the macro's definition

  A macro's hover card carries its `#define` text wherever the name
  appears: the definition itself, a use, an `#ifdef` guard and an `#undef`.

  <details>
  <summary>Example</summary>

  ```cpp
  int anchor = 0;

  #define LIMIT 64

  int use = LIMIT;

  #ifdef LIMIT
  int guarded = 1;
  #endif

  #undef LIMIT
  ```

  </details>

- [x] Fully-expanded preview — a function-like macro use shows its arguments substituted through the body

  Hovering a function-like macro invocation shows the `#define` text and a
  preview of the fully-expanded result with the call's arguments spliced in.

  <details>
  <summary>Example</summary>

  ```cpp
  int x = 1, y = 2;

  #define MAX(a, b) ((a) > (b) ? (a) : (b))

  int z = MAX(x, y);
  ```

  </details>

- [x] Command-line macros — `-D` definitions hover with a synthesized `#define`

  A macro defined on the command line (`-DFROM_CLI=7`) shows a synthesized
  `#define FROM_CLI 7` in its hover card, then its expansion.

  <details>
  <summary>Example</summary>

  ```cpp
  int cli = FROM_CLI;
  ```

  </details>

- [ ] Nested macro in arguments — a macro named inside another invocation's arguments _(partial)_

  The recorded expansion starts at the outer invocation, so hovering an
  inner macro named inside the arguments shows only its definition, not an
  expansion preview.

  <details>
  <summary>Example</summary>

  ```cpp
  int anchor = 0;

  #define ECHO(x) x
  #define INNER_VAL 99

  int nested = ECHO(INNER_VAL);
  ```

  </details>

- [ ] Use before definition — hovering a macro name that appears before its `#define` _(partial)_ ([clangd#2642](https://github.com/clangd/clangd/issues/2642))

  A macro name used in an `#if` above its own `#define` should still hover
  with the macro's definition. clice currently returns no hover at the
  pre-definition use; a use after the `#define` works normally.

  <details>
  <summary>Example</summary>

  ```cpp
  int anchor = 0;

  #if COUNT > 0
  int positive = 1;
  #endif

  #define COUNT 3

  int use = COUNT;
  ```

  </details>

- [ ] `#define` inside the preamble — hover on a leading directive

  A `#define` in the file's preamble region (the leading run of directives
  before the first declaration) is not part of the live parse's
  preprocessor record, so hovering its name yields nothing. Every other
  macro fixture opens with a declaration precisely to push its directives
  past the preamble boundary.

  <details>
  <summary>Example</summary>

  ```cpp
  #define EARLY 1

  int use = EARLY;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Special Hover Targets

<!-- BEGIN GENERATED ITEMS: Special Hover Targets -->

- [ ] Members on type hover — hovering an enum or struct type lists its members _(partial)_ ([clangd#959](https://github.com/clangd/clangd/issues/959))

  The card names the type (and a struct's layout), but the member list is
  not expanded — the body renders as `{}`.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace members {

  enum Color {
      Red,
      Green,
      Blue,
  };

  struct Point {
      int x;
      int y;
  };

  }
  ```

  </details>

- [ ] Typedef underlying struct — hovering an alias expands the aliased definition _(partial)_ ([clangd#2020](https://github.com/clangd/clangd/issues/2020))

  The card resolves the alias to its underlying type name, but does not
  expand that struct's definition or member list.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace aliases {

  struct Widget {
      int id;
      double value;
  };

  using Handle = Widget;

  typedef Widget Widget_t;

  }
  ```

  </details>

- [ ] Keyword documentation — hovering a language keyword shows its description ([clangd#1862](https://github.com/clangd/clangd/issues/1862))

  Hovering a keyword such as `const` or `virtual` produces no card.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace keywords {

  const int limit = 42;

  struct Widget {
      virtual void draw();
  };

  }
  ```

  </details>

- [x] Attribute documentation — hovering an attribute shows its description ([clangd#1862](https://github.com/clangd/clangd/issues/1862))

  The attribute's own documentation renders in the card, for both GNU
  `__attribute__` spellings and C++ `[[...]]` attributes.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace attr_docs {
  void foo(int * __attribute__((nonnull, noescape)) );

  [[nodiscard]] int compute();
  }
  ```

  </details>

- [x] Include directive hover — hovering an `#include` shows the resolved header path

  The card resolves the quoted header to its file on disk.

  <details>
  <summary>Example</summary>

  ```cpp
  #include "own_header.h"

  int use = own_header_value;
  ```

  </details>

- [x] `this` expression — hovering `this` shows the pointed-to class type

  Works in a plain class and inside a class template.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace this_hover {

  struct Widget {
      Widget* self() {
          return this;
      }
  };

  template <typename T>
  struct Box {
      const Box* self() const {
          return this;
      }
  };

  }
  ```

  </details>

- [x] Predefined identifiers — `__func__` hover shows the current function name

  The value resolves in a concrete function; inside a template only the
  approximate type is known.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace predefined {

  void current() {
      const char* name = __func__;
  }

  template <int N>
  void generic() {
      const char* name = __func__;
  }

  }
  ```

  </details>

- [x] No hover on meaningless tokens — builtin keywords and empty bodies yield no card

  Hovering a builtin type keyword or the inside of an empty body
  produces no card at all, so editors show nothing rather than noise.
  (Numeric and bool literals also have no card today, but that is a
  tracked gap — see the numeric-literal item — not a promise.)

  <details>
  <summary>Example</summary>

  ```cpp
  namespace negatives {

  int counter = 0;

  void noop() {}

  }
  ```

  </details>

- [ ] GTK-Doc and kernel-doc — recognize GObject Introspection annotations ([clangd#2662](https://github.com/clangd/clangd/issues/2662))

  GTK-Doc / kernel-doc comment syntax and GObject Introspection
  annotations are not parsed into the hover card.

  <details>
  <summary>Example</summary>

  ```cpp
  /**
   * gtk_widget_show:
   * @widget: (transfer none): a #GtkWidget
   *
   * Flags a widget to be displayed.
   */
  void gtk_widget_show(GtkWidget *widget);
  ```

  </details>

- [ ] LaTeX math in Doxygen — render `@f$ ... @f$` formulas ([clangd#2669](https://github.com/clangd/clangd/issues/2669))

  Doxygen LaTeX math formulas are shown verbatim, not rendered as math.

  <details>
  <summary>Example</summary>

  ```cpp
  /// The area of a circle is @f$ A = \pi r^2 @f$.
  double circle_area(double r);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: Presentation -->

- [x] Markdown rendering — cards render as markdown, or plain text via `parse_comment_as_markdown = false`

  <details>
  <summary>Example</summary>

  ```cpp
  /// Computes the answer. Tests primality of `p`.
  constexpr int answer(int p) {
      return p + 41;
  }

  int value = answer(1);

  struct Layout {
      char first;
      int second;
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Module-Related

<!-- BEGIN GENERATED ITEMS: Module-Related -->

- [ ] Import statement hover — hovering `import` shows the module's info

  Hovering an `import` declaration does not yet describe the imported
  module.

  <details>
  <summary>Example</summary>

  ```cpp
  export module app;

  import utils;
  ```

  </details>

- [ ] Module name hover — hovering a module name lists its owning files

  Hovering a module name does not yet list the files or partitions that
  declare it.

  <details>
  <summary>Example</summary>

  ```cpp
  export module math;

  export module math:algebra;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Hover Correctness

Robustness on inputs that have broken other tooling.

<!-- BEGIN GENERATED ITEMS: Hover Correctness -->

- [x] MSVC inheritance model — `MSInheritanceAttr` does not corrupt record hover

  clangd tracks this as clangd#1643 and clangd#2212; under an MSVC target
  the implicit inheritance attribute does not leak into the record or
  method card.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace ms {

  struct Widget {
      int value;
      void update();
  };

  int Widget::* member = &Widget::value;

  }
  ```

  </details>

- [x] Most-vexing-parse — object init and function declaration hover distinctly

  clangd tracks this as clangd#2225; clice reads the direct-init as a
  variable and the vexing form as a function declaration.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace mvp {

  struct Timer {
      Timer();
      Timer(int);
  };

  int seconds = 5;

  void demo() {
      Timer active(seconds);
      Timer empty();
  }

  }
  ```

  </details>

- [x] Large unsigned enum constant — hovering a `0xFFFF...ULL` enumerator does not crash

  clangd crashes on this (clangd#2381); clice renders the full unsigned
  value without overflow.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace big_enum {

  enum class Flags : unsigned long long {
      Max = 0xFFFFFFFFFFFFFFFFULL,
  };

  }
  ```

  </details>

- [x] Call with default arguments — hovering a call that omits defaults does not crash

  clangd crashes on this (clangd#551); clice renders the callee signature
  with its default arguments.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace defaults {

  int compute(int a, int b = 10, int c = 20);

  int result = compute(1);

  }
  ```

  </details>

- [x] Macro-shadowed symbol — a function-like macro over a same-named function

  clangd tracks this as clangd#2490; at the call site the function-like
  macro is active, and clice's card shows that macro and its expansion.

  <details>
  <summary>Example</summary>

  ```cpp
  namespace shadow {

  int lookup(int key) {
      return key;
  }

  }

  #define lookup(key) ((key) + 100)

  int value = lookup(5);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Changelog

| Date       | Change                                                         | PR                                                 |
| ---------- | -------------------------------------------------------------- | -------------------------------------------------- |
| 2026-08-23 | Fixture-generated checklist; corpus reorganized by card aspect | [#633](https://github.com/clice-io/clice/pull/633) |
| 2026-08-23 | Macro hover: definitions, expansion preview                    | [#629](https://github.com/clice-io/clice/pull/629) |
| 2026-08-21 | Resolved paths for include and embed directives                | [#581](https://github.com/clice-io/clice/pull/581) |
| 2026-06-12 | Port clangd hover implementation                               | [#452](https://github.com/clice-io/clice/pull/452) |
