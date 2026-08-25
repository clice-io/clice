# Code Navigation

## Go to Definition

<!-- BEGIN GENERATED ITEMS: Go to Definition -->

- [x] Index-based cross-TU go-to-definition

  A use in one translation unit resolves to a definition supplied by a
  sibling source, drawing on the project-wide index rather than the
  current file alone.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  #include "shared.h"

  int run(int value) {
      return transform(value);
  }
  ```

  `lib.cpp`:

  ```cpp
  #include "shared.h"

  int transform(int value) {
      return value * 2;
  }
  ```

  `shared.h`:

  ```cpp
  #pragma once

  int transform(int value);
  ```

  </details>

- [x] Definition and declaration alternate at the cursor site

  On a use, go-to-definition reaches the definition. Invoked on the
  definition it steps to the declaration, and on the declaration it
  steps to the definition — the two sites alternate. A symbol defined
  inline, with no separate declaration, keeps its definition as the
  answer.

  <details>
  <summary>Example</summary>

  ```cpp
  int scale(int value);

  int scale(int value) {
      return value * 2;
  }

  int apply(int value) {
      return scale(value);
  }
  ```

  </details>

- [x] Declaration-only symbols navigate to their declaration

  Symbols that carry only a declaration — pure virtuals, `extern`
  variables, out-of-line static constants — resolve to that declaration
  instead of returning nothing.

  <details>
  <summary>Example</summary>

  ```cpp
  extern int threshold;

  int probe(int value);

  int watch(int value) {
      return probe(value) + threshold;
  }
  ```

  </details>

- [x] Go-to-definition on `#include` directives

  Invoked on an `#include` line, go-to-definition opens the included
  file. This works for the leading includes compiled into the preamble
  (the PCH) as well as ordinary ones.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  #include "panel.h"

  int build() {
      return dimension();
  }
  ```

  `panel.h`:

  ```cpp
  #pragma once

  int dimension();
  ```

  </details>

- [x] Local variables and parameters navigate to their declaration

  Go-to-definition on a local variable or parameter jumps to its
  declaration inside the function body, resolved from the in-memory AST
  without any index — the same resolution that keeps working while a
  buffer has unsaved edits.

  <details>
  <summary>Example</summary>

  ```cpp
  int accumulate(int base) {
      int total = base;
      total = total + base;
      return total;
  }
  ```

  </details>

- [x] Navigate through macro wrappers to the underlying declaration

  A name spelled in a macro argument anchors at its spelling, so
  definition and declaration alternate there exactly as at a plain
  site, and a later use resolves through the wrapper to the function it
  declares.

  <details>
  <summary>Example</summary>

  ```cpp
  #define DECLARE_HOOK(name) int name(int value)

  DECLARE_HOOK(notify);

  DECLARE_HOOK(notify) {
      return value + 1;
  }

  int trigger(int value) {
      return notify(value);
  }
  ```

  </details>

- [x] Names conjured by a macro body or token paste anchor at the invocation

  A name assembled by token paste has no spelling of its own in the
  source, so it anchors at the macro invocation that creates it: the
  invocation is its definition site, and a plain use of the name jumps
  back to that invocation.

  <details>
  <summary>Example</summary>

  ```cpp
  #define MAKE_FLAG(name) bool flag_##name = false

  MAKE_FLAG(verbose);

  bool read_flag() {
      return flag_verbose;
  }
  ```

  </details>

- [x] Tokens inside a `#define` body carry no navigation of their own

  A token written inside a macro body has no meaning until an expansion
  assigns one, so navigation on it yields nothing, while the invocation
  token always resolves to the macro being expanded.

  <details>
  <summary>Example</summary>

  ```cpp
  #define DEFINE_COUNTER int counter = 0

  DEFINE_COUNTER;
  ```

  </details>

- [ ] Error recovery — navigate to a variable whose type is unresolved

  When a variable's type name fails to resolve, go-to-definition on a
  later use of the variable currently returns nothing, even though the
  variable's own declaration is still recorded.

  <details>
  <summary>Example</summary>

  ```cpp
  Unresolved handle;  // 'Unresolved' does not name a type

  void read() {
      (void) handle;  // go-to-def on handle → the declaration above
  }
  ```

  </details>

- [x] Dependent member navigation in uninstantiated templates

  Inside a template that is never instantiated, a member accessed on an
  object of a dependent type resolves to the member declared on the
  corresponding class template.

  <details>
  <summary>Example</summary>

  ```cpp
  template <typename T>
  struct Sink {
      void push(T value);
  };

  template <typename T>
  void drain(Sink<T>& sink, T value) {
      sink.push(value);
  }
  ```

  </details>

- [ ] Template specialization navigates to the primary template ([clangd#212](https://github.com/clangd/clangd/issues/212))

  Go-to-definition on the name of an explicit specialization resolves to
  the specialization itself; stepping from it to the primary template it
  specializes is not offered.

  <details>
  <summary>Example</summary>

  ```cpp
  template <typename T>
  struct Formatter {}; // primary template

  template <>
  struct Formatter<int> {}; // go-to-def on Formatter → primary template
  ```

  </details>

- [ ] `auto` keyword navigates to the deduced type ([clangd#2055](https://github.com/clangd/clangd/issues/2055))

  Go-to-definition on the `auto` keyword should reach the type it was
  deduced to; today it returns nothing.

  <details>
  <summary>Example</summary>

  ```cpp
  struct Widget {};

  Widget make_widget();

  void use() {
      auto widget = make_widget(); // go-to-def on auto → Widget
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

### Implicit Code Navigation

Navigate to definitions of implicitly invoked code. In C++ many constructs generate hidden calls to constructors, operators, conversions, etc. Navigating from the syntactic construct (a brace, a keyword, an operator token) to the actual function being called is essential for understanding what code is really executing.

Implicit navigation requires an unambiguous source token — patterns where the token already has a well-defined go-to-def target (e.g., a variable name always goes to its declaration) cannot be repurposed for implicit call navigation.

**Keywords**

- [ ] `override` / `final` → the overridden base class virtual method

  ```cpp
  struct Base { virtual void draw(); };
  struct Derived : Base {
      void draw() override;  // go-to-def on override → Base::draw
  };
  ```

- [ ] `break` / `continue` → enclosing loop or switch head ([clangd#1921](https://github.com/clangd/clangd/issues/1921)). See also [Document Highlight](#document-highlight) for highlighting all related control flow tokens in context.

**Construction & destruction**

- [ ] Constructor calls — from parentheses/braces to the selected constructor

  ```cpp
  struct Widget { Widget(int w, int h); };
  Widget w(800, 600);        // go-to-def on ( → Widget(int, int)
  Widget w2{800, 600};       // go-to-def on { → same
  auto w3 = Widget(1, 2);   // go-to-def on ( → same
  ```

- [ ] Copy/move construction and assignment

  ```cpp
  Widget a(1, 2);
  Widget b = a;              // go-to-def on = → Widget(const Widget&)
  Widget c = std::move(a);   // go-to-def on = → Widget(Widget&&)
  b = c;                     // go-to-def on = → operator=(const Widget&)
  ```

- [ ] CTAD — navigate to the selected constructor

  ```cpp
  std::vector v{1, 2, 3};  // go-to-def on { → vector(initializer_list<int>)
  ```

- [ ] Aggregate initialization → struct definition

  ```cpp
  struct Point { int x, y; };
  auto p = Point{1, 2};  // go-to-def on { → Point
  ```

- [ ] `delete` expression → destructor

  ```cpp
  delete widget;  // go-to-def on delete → Widget::~Widget
  ```

- [ ] `new` expression → constructor (and custom `operator new` if overloaded)

  ```cpp
  struct Pool {
      static void* operator new(size_t);
  };
  auto* p = new Pool();       // go-to-def on new → Pool() constructor
                               // also: Pool::operator new (if overloaded)
  auto* arr = new Pool[10];   // go-to-def on new → Pool() default constructor
  ```

- [ ] Member initializer list → base class and member constructors

  ```cpp
  struct Base { Base(int); };
  struct Logger { Logger(std::string name); };
  struct App : Base {
      Logger logger;
      App() : Base(42), logger("app") {}
      // go-to-def on Base → Base::Base(int)
      // go-to-def on logger → Logger(std::string)
  };
  ```

- [ ] Delegating constructors

  ```cpp
  struct Widget {
      Widget(int w, int h);
      Widget() : Widget(0, 0) {}  // go-to-def on Widget → Widget(int, int)
  };
  ```

- [ ] Inherited constructors — navigate to the base constructors brought in by `using`

  ```cpp
  struct Base { Base(int x); Base(int x, int y); };
  struct Derived : Base {
      using Base::Base;  // go-to-def on Base::Base → list Base's constructors
  };
  ```

- [ ] Return value implicit construction

  ```cpp
  Widget create() {
      return {800, 600};  // go-to-def on { → Widget(int, int)
  }
  ```

- [ ] Lambda init-capture → constructor

  ```cpp
  Widget w;
  auto f = [w = std::move(w)] {};     // go-to-def on = → Widget(Widget&&)
  auto g = [s = std::string("hi")] {};  // go-to-def on = → string(const char*)
  ```

**Operators**

- [ ] Overloaded operators — from the operator token to its definition

  ```cpp
  Vec a, b;
  auto c = a + b;   // go-to-def on + → Vec::operator+
  a += b;            // go-to-def on += → Vec::operator+=
  ++it;              // go-to-def on ++ → iterator::operator++
  v[0];              // go-to-def on [ → vector::operator[]
  fn(42);            // go-to-def on ( → Functor::operator()
  ptr->member;       // go-to-def on -> → SmartPtr::operator->
  ```

- [ ] C++20 rewritten operators — navigate to the actual operator used by the rewrite

  ```cpp
  struct S {
      bool operator==(const S&) const;
      auto operator<=>(const S&) const = default;
  };
  S a, b;
  a != b;   // go-to-def on != → S::operator==
  a > b;    // go-to-def on > → S::operator<=>
  ```

- [ ] User-defined literals

  ```cpp
  using namespace std::chrono_literals;
  auto d = 500ms;  // go-to-def on ms → operator""ms
  ```

**Conversions**

- [ ] Implicit conversion operators — from contexts where a conversion is invoked

  ```cpp
  struct Guard { explicit operator bool() const; };
  Guard g;
  if (g) {}              // go-to-def on ( → Guard::operator bool()
  while (g) {}           // same
  !g;                    // go-to-def on ! → Guard::operator bool()  ([clangd#1931](https://github.com/clangd/clangd/issues/1931))
  bool ok = bool(g);     // go-to-def on bool( → Guard::operator bool()
  ```

- [ ] Casts invoking constructor or conversion operator

  ```cpp
  struct Meters { explicit operator double() const; };
  Meters m;
  double d = static_cast<double>(m);  // go-to-def on static_cast → Meters::operator double()

  struct Foo { explicit Foo(int); };
  auto f = static_cast<Foo>(42);      // go-to-def on static_cast → Foo(int)
  ```

**Range-for & structured bindings**

- [ ] Range-based for — navigate to `begin()`/`end()`

  ```cpp
  std::vector<int> v;
  for (auto& x : v) {}  // go-to-def on : → vector::begin
  ```

- [ ] Structured bindings — navigate to the underlying accessors or fields

  ```cpp
  std::map<int, std::string> m;
  for (auto& [key, val] : m) {}  // go-to-def on key → pair::first
                                  // go-to-def on val → pair::second
  ```

**Coroutines**

- [ ] `co_await` / `co_yield` / `co_return` → the corresponding awaiter/promise method

  ```cpp
  co_await some_awaitable;   // go-to-def on co_await → operator co_await() or await_resume()
  co_yield value;            // go-to-def on co_yield → promise::yield_value()
  co_return result;          // go-to-def on co_return → promise::return_value()
  ```

## Go to Declaration

Navigate from a symbol usage or definition to its declaration. In C++, many entities have separate declarations and definitions.

clice returns the declaration locations plus the definition — symbols defined inline have no separate declaration — minus the site the cursor already stands on, so declaration and definition sites alternate just like go-to-definition.

- [x] Index-based cross-TU go-to-declaration
- [x] Functions — from usage or out-of-line definition to the declaration/prototype

  ```cpp
  // widget.h
  class Widget {
      void draw();  // declaration
  };

  // widget.cpp
  void Widget::draw() { }  // out-of-line definition
  // go-to-decl from usage or definition → in-class declaration in widget.h
  ```

- [ ] Forward declarations of classes and structs

  ```cpp
  class Widget;           // forward declaration in fwd.h
  class Widget { ... };   // full definition in widget.h
  // go-to-decl on Widget (from usage or definition) → forward declaration
  ```

- [ ] Static data member → in-class declaration

  ```cpp
  struct Config {
      static int timeout;    // declaration
  };
  int Config::timeout = 30;  // out-of-class definition
  // go-to-decl on timeout → in-class declaration
  ```

- [ ] `extern` variable → declaration

  ```cpp
  // globals.h
  extern int log_level;      // declaration
  // globals.cpp
  int log_level = 0;         // definition
  // go-to-decl on log_level → extern declaration in globals.h
  ```

- [ ] Multiple declarations — list all when an entity is declared in more than one location
- [ ] Navigate even when declaration and definition signatures mismatch (e.g., parameter names differ, const qualification)

## Go to Implementation

- [x] Index-based go-to-implementation (direct overrides; each level of a
      deep chain navigates to its own overriders)
- [x] Virtual method → all override implementations

  ```cpp
  struct Base { virtual void draw(); };
  struct Circle : Base { void draw() override; };
  struct Rect : Base { void draw() override; };
  // go-to-impl on Base::draw → list Circle::draw, Rect::draw
  ```

- [ ] Non-virtual function declaration → out-of-line definition (go-to-impl as superset of go-to-def) ([clangd#854](https://github.com/clangd/clangd/issues/854))

  ```cpp
  // widget.h
  class Widget {
      void draw();  // go-to-impl on draw → out-of-line definition in widget.cpp
  };
  ```

- [ ] Go to implementation listing all derived classes for a base class

  ```cpp
  struct Base {};
  struct Circle : Base {};
  struct Rect : Base {};
  // go-to-impl on Base → list Circle, Rect
  ```

- [ ] Template duck-type navigation — when a template has known instantiations, jump to the concrete implementations of dependent member calls. Also applies to generic lambdas with known call sites.

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo();  // go-to-impl on foo → list A::foo, B::foo (from all instantiations)
  }

  process(a);  // T = A
  process(b);  // T = B
  ```

  ```cpp
  std::vector<std::string> names;
  std::ranges::for_each(names, [](const auto& s) {
      s.size();  // go-to-impl on size → std::string::size (from the single call site)
  });
  ```

## Go to Type Definition

Navigate to the type definition of a symbol. Applicable to variables, parameters, fields, and any other named entity that has a type. When the type is a type alias or a pointer-like wrapper, navigation should unwrap to the underlying/pointee type.

- [x] Index-based go-to-type-definition for declared entities (variables,
      parameters, fields). Known limitations: `auto`-deduced variables have no
      type relation yet, and alias-typed variables navigate to the `using`
      declaration rather than unwrapping to the underlying type.
- [x] Local variables and parameters

  ```cpp
  void process(Widget w) {
      auto result = w.compute();
      // go-to-type-def on w → Widget
      // go-to-type-def on result → return type of compute()
  }
  ```

- [ ] Class/struct fields

  ```cpp
  struct App { Logger logger; };
  App app;
  // go-to-type-def on app.logger → Logger
  ```

- [ ] `auto` deduced types

  ```cpp
  auto it = map.begin();
  // go-to-type-def on it → map::iterator
  ```

- [ ] Smart pointer → pointee type ([clangd#1026](https://github.com/clangd/clangd/issues/1026))

  ```cpp
  std::unique_ptr<Widget> w;
  // go-to-type-def on w → Widget, not unique_ptr
  ```

- [ ] Type aliases — unwrap `typedef` / `using` to the underlying type definition (behavior may depend on cursor position and context, e.g. whether the cursor is on the variable or on the alias name itself)

  ```cpp
  using Connection = detail::ConnectionImpl;
  Connection conn;
  // go-to-type-def on conn → detail::ConnectionImpl (unwraps alias)
  ```

- [ ] Structured binding variables

  ```cpp
  std::map<int, Widget> m;
  auto& [id, widget] = *m.begin();
  // go-to-type-def on id → int (from pair::first)
  // go-to-type-def on widget → Widget (from pair::second)
  ```

## Find References

- [x] Index-based cross-TU find references
- [x] Include declarations option
- [ ] Implicit references from range-based for loops ([clangd#1081](https://github.com/clangd/clangd/issues/1081))

  ```cpp
  struct Container { iterator begin(); iterator end(); };
  for (auto& x : container) {}  // find-refs on begin() should include this loop
  ```

- [ ] Implicit constructor/destructor calls

  ```cpp
  struct Blob { Blob(); };
  Blob b;  // find-refs on Blob() should include this declaration
  ```

- [ ] References through forwarding functions — find-refs on a constructor should include calls via `std::make_unique`, `std::make_shared`, `emplace_back`, etc. ([clangd#716](https://github.com/clangd/clangd/issues/716), [clangd#1872](https://github.com/clangd/clangd/issues/1872))

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(800, 600);  // find-refs on Widget(int, int) should include this
  vec.emplace_back(800, 600);                    // and this
  ```

- [ ] References in dependent/template contexts ([clangd#258](https://github.com/clangd/clangd/issues/258), [clangd#675](https://github.com/clangd/clangd/issues/675))

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo();  // find-refs on A::foo should include this (from instantiation T = A)
  }
  ```

- [ ] Read/write classification — annotate each reference as read or write access ([clangd#2139](https://github.com/clangd/clangd/issues/2139))

  ```cpp
  int x = 0;        // write
  int y = x + 1;    // read
  x = y;            // write
  ```

- [ ] Enclosing function context — include the name of the enclosing function in each reference result for better readability ([clangd#177](https://github.com/clangd/clangd/issues/177))
- [x] Macro references across expansion, `#ifdef`/`#ifndef` and `#undef` sites; each `#define` of a name is its own symbol
- [ ] Macro references spelled inside other macro definitions ([clangd#346](https://github.com/clangd/clangd/issues/346))
- [ ] Label → goto references

  ```cpp
  retry:
    if (failed) goto retry;  // find-refs on retry label → list all gotos
  ```

## Call Hierarchy

- [x] Prepare call hierarchy (functions and methods)
- [x] Incoming calls
- [x] Outgoing calls
- [ ] Show function signature in `detail` field
- [ ] Include class name for member functions

  ```
  // current:  "draw" in file.cpp
  // expected: "Circle::draw" in file.cpp
  ```

- [ ] Follow virtual dispatch (callers of `Base::draw` should include calls via derived overrides)
- [ ] Support non-function targets (variables, enum constants) ([clangd#1308](https://github.com/clangd/clangd/issues/1308))
- [ ] Calls inside lambdas

  ```cpp
  auto task = [&] { foo(); };  // foo() should appear in foo's incoming calls
  ```

- [ ] Constructor calls through forwarding functions — `make_unique`, `emplace_back` etc. should appear in incoming calls of the constructor ([clangd#2242](https://github.com/clangd/clangd/issues/2242))

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(800, 600);
  // incoming calls for Widget(int, int) should include this call site
  ```

## Type Hierarchy

- [x] Prepare type hierarchy (class/struct/enum/union)
- [x] Supertypes (base classes)
- [x] Subtypes (derived classes)
- [ ] Template inheritance (derived classes via template specialization)

  ```cpp
  template<typename T>
  struct CRTP : Base {};
  // type hierarchy on Base should show CRTP<T> as subtype
  ```

- [ ] Show template arguments in type hierarchy items ([clangd#31](https://github.com/clangd/clangd/issues/31))

  ```
  // current:  CRTP (subtype of Base)
  // expected: CRTP<Foo> (subtype of Base)
  ```

## Workspace Symbol

Search the whole project for a symbol by name (`workspace/symbol`).

<!-- BEGIN GENERATED ITEMS: Workspace Symbol -->

- [x] Basic workspace-wide symbol search — case-insensitive substring over all symbol kinds

  A query matches any symbol whose name contains it, ignoring case:
  functions, types, enumerators and macros all participate, and a query
  with no match returns an empty list rather than an error.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: widget
  // query: parse_config
  // query: MODE
  // query: fast
  // query: no_such_symbol

  struct Widget {
      int width;
  };

  enum class Mode { Fast, Safe };

  #define MODE_DEFAULT 1

  void parse_config() {}
  ```

  </details>

- [x] Search spans the whole project — hits from files other than the queried one

  The search space is the project, not one buffer: a query typed while
  editing `main.cpp` still surfaces symbols defined in other sources.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  // query: helper_elsewhere

  int local_anchor = 0;
  ```

  `other.cpp`:

  ```cpp
  void helper_elsewhere() {}
  ```

  </details>

- [ ] Overload disambiguation — parameter types shown in results _(partial)_ ([clangd#1344](https://github.com/clangd/clangd/issues/1344))

  Querying an overloaded name finds every overload, but each entry
  carries only the bare name — nothing tells the two `process` results
  apart short of opening both locations.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: process

  void process(int value) {}

  void process(bool flag, int level) {}
  ```

  </details>

- [ ] Fuzzy matching — word-boundary-aware scoring for camelCase and snake_case ([clangd#914](https://github.com/clangd/clangd/issues/914))

  Matching is a case-insensitive substring test: `LinLis` does not find
  `LinkedList`, and `pcfg` does not find `parse_config`. Word-boundary
  initials should match and score for every symbol kind, macros
  included.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: LinLis
  // query: pcfg

  struct LinkedList {};

  void parse_config();
  ```

  </details>

- [ ] Partially qualified name search ([clangd#550](https://github.com/clangd/clangd/issues/550))

  Symbols match by bare name only: `net::Socket` finds nothing even
  though `deep::net::Socket` exists, and neither does any other
  qualifier-prefixed form.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: net::Socket

  namespace deep {
  namespace net {

  struct Socket {};

  }  // namespace net
  }  // namespace deep
  ```

  </details>

- [ ] Enumerator lookup under the enum's scope ([clangd#931](https://github.com/clangd/clangd/issues/931))

  `Color::Red` should find the enumerator — for scoped and unscoped
  enums alike — but qualified queries match nothing; only the bare
  `Red` does.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: Color::Red

  enum Color { Red, Green };
  ```

  </details>

- [ ] Underlying declarations ranked above type aliases ([clangd#2253](https://github.com/clangd/clangd/issues/2253))

  When both `ConnectionImpl` and its alias `Connection` match a query,
  the underlying declaration should rank first. Results carry no
  ranking today.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: Connection

  struct ConnectionImpl {};

  using Connection = ConnectionImpl;
  ```

  </details>

- [ ] Search by mangled (linker) name

  Pasting a linker symbol such as `_Z7processi` should resolve to the
  function it mangles — useful when chasing linker errors and stack
  traces.

  <details>
  <summary>Example</summary>

  ```cpp
  // query: _Z7processi

  void process(int value);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Module Navigation

<!-- BEGIN GENERATED ITEMS: Module Navigation -->

- [x] `import module_name` navigates to the module interface unit ([clangd#2310](https://github.com/clangd/clangd/issues/2310))

  Go-to-definition on the name in an `import` declaration opens the
  module interface unit that exports it, and uses of an imported symbol
  reach its definition in that unit.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  import widget;

  int build() {
      return area(2, 3);
  }
  ```

  `widget.cppm`:

  ```cpp
  export module widget;

  export int area(int width, int height) {
      return width * height;
  }
  ```

  </details>

- [x] `import :partition` navigates to the partition unit

  Go-to-definition on the partition name after the colon in a partition
  import opens the partition unit that declares it.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  import pack;

  int run() {
      return count();
  }
  ```

  `pack.cppm`:

  ```cpp
  export module pack;

  export import :items;
  ```

  `pack_items.cppm`:

  ```cpp
  export module pack:items;

  export int count() {
      return 3;
  }
  ```

  </details>

- [ ] Navigate between interface and implementation units of one module _(partial)_

  Go-to-definition on the module name in an implementation unit
  (`module m;`) jumps to the interface unit that declares the module;
  the reverse direction, from the interface name to the implementation,
  is not offered.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  import store;

  int lookup(int key) {
      return fetch(key);
  }
  ```

  `iface.cppm`:

  ```cpp
  export module store;

  export int fetch(int key);
  ```

  `impl.cpp`:

  ```cpp
  module store;

  int fetch(int key) {
      return key * 2;
  }
  ```

  </details>

- [ ] Dot-separated module name — navigate each segment _(partial)_

  Go-to-definition on the leading segment of a dot-separated module name
  reaches the module's interface unit; the segments after a dot do not
  resolve on their own yet.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  import app.core;

  int run() {
      return value();
  }
  ```

  `app_core.cppm`:

  ```cpp
  export module app.core;

  export int value() {
      return 1;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Document Highlight

Highlight all references to the symbol under cursor within the current file (`textDocument/documentHighlight`).

<!-- BEGIN GENERATED ITEMS: Document Highlight -->

- [ ] Highlight every reference to the symbol under the cursor in the current file

  Placing the cursor on `total` should light up its declaration and
  every use in the file; the request is not implemented.

  <details>
  <summary>Example</summary>

  ```cpp
  int total = 0;

  void accumulate(int amount) {
      total = total + amount;
  }
  ```

  </details>

- [ ] Read/write classification for symbol highlights

  Each highlight should carry its access kind, so editors can tint
  writes differently from reads.

  <details>
  <summary>Example</summary>

  ```cpp
  void tally() {
      int count = 0;      // write
      int next = count;   // read
      count = next;       // write
  }
  ```

  </details>

- [ ] Control flow token highlighting ([clangd#1921](https://github.com/clangd/clangd/issues/1921))

  Highlighting `break` or `continue` should also light up the loop or
  `switch` it belongs to — and `return` / `throw` the function exits
  they mark.

  <details>
  <summary>Example</summary>

  ```cpp
  void drain(int outer, int inner) {
      for (int i = 0; i < outer; i += 1) {
          for (int j = 0; j < inner; j += 1) {
              if (i == j) {
                  break;      // highlighting break → also the inner for
              }
              if (j == 0) {
                  continue;   // highlighting continue → also the inner for
              }
          }
      }
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Switch Source/Header

<!-- BEGIN GENERATED ITEMS: Switch Source/Header -->

- [ ] Switch between a source file and its header

  From `widget.cpp` a single command should jump to `widget.h` and
  back — the `textDocument/switchSourceHeader` request clangd clients
  rely on is not implemented.

  <details>
  <summary>Example</summary>

  ```cpp
  // widget.h
  class Widget {
      void draw();
  };

  // widget.cpp — #include "widget.h"
  void Widget::draw() {}
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Changelog

| Date       | Change                                                                                             | PR                                                 |
| ---------- | -------------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| 2026-08-22 | definition/declaration alternate at the cursor site; declaration-only symbols serve declarations   | [#626](https://github.com/clice-io/clice/pull/626) |
| 2026-07-04 | go-to-definition on include directives and module names                                            | [#481](https://github.com/clice-io/clice/pull/481) |
| 2026-07-03 | declaration / implementation / typeDefinition; references includeDeclaration includes declarations | [#480](https://github.com/clice-io/clice/pull/480) |
| 2026-04-02 | Index-based go-to-definition and find references; call hierarchy; type hierarchy                   | [#382](https://github.com/clice-io/clice/pull/382) |
