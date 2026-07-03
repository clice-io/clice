# Code Navigation

## Go to Definition

- [x] Index-based cross-TU go-to-definition
- [x] Go to definition on `#include` directives (navigate to the included file), including preamble includes compiled into the PCH
- [ ] AST-based fallback for local/unsaved symbols
- [ ] Navigate through macro wrappers to the underlying declaration

  ```cpp
  #define DECLARE_HANDLER(name) void name()
  DECLARE_HANDLER(onReady);  // go-to-def on onReady → should reach the underlying function
  ```

- [ ] Error recovery: navigate to variable definition even when its type is unresolved
- [ ] Dependent type navigation in uninstantiated templates

  ```cpp
  template<typename T>
  void process(std::vector<T>& v) {
      v.push_back(val);  // go-to-def on push_back → vector::push_back
  }
  ```

- [ ] Template specialization → primary template ([clangd#212](https://github.com/clangd/clangd/issues/212))

  ```cpp
  template<typename T>
  struct Formatter { ... };           // primary template

  template<>
  struct Formatter<std::string> { };  // go-to-def on Formatter → primary template
  ```

- [ ] `auto` keyword → deduced type ([clangd#2055](https://github.com/clangd/clangd/issues/2055))

  ```cpp
  auto widget = getWidget();
  // go-to-def on auto → Widget (the deduced type)
  ```

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

Navigate from a symbol usage or definition to its declaration. In C++, many entities have separate declarations and definitions; go-to-declaration always targets the declaration side.

clice returns the declaration locations plus the definition: symbols defined inline have no separate declaration, and navigating to the definition is what clients expect in that case.

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
- [ ] Macro definition/expansion references (including uses within other macro definitions) ([clangd#346](https://github.com/clangd/clangd/issues/346))
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

- [ ] Basic workspace-wide symbol search
- [ ] Fuzzy matching with word-boundary-aware scoring (camelCase, snake_case)

  ```
  VecIt → matches std::vector<int>::iterator
  strvi → matches std::string_view
  ```

- [ ] Partially qualified name search ([clangd#550](https://github.com/clangd/clangd/issues/550))

  ```
  ns::Foo → matches deeply::nested::ns::Foo
  ```

- [ ] Include parameter types for overloaded function disambiguation ([clangd#1344](https://github.com/clangd/clangd/issues/1344))

  ```
  // query: "process"
  // results: process(int), process(std::string), process(Widget&)
  ```

- [ ] Enum enumerator lookup under enum scope ([clangd#931](https://github.com/clangd/clangd/issues/931))

  ```
  Color::Red → matches Color::Red (even for unscoped enums)
  ```

- [ ] Fuzzy matching for macros (consistent with other symbol types) ([clangd#914](https://github.com/clangd/clangd/issues/914))
- [ ] Prioritize underlying declarations over type aliases in results ([clangd#2253](https://github.com/clangd/clangd/issues/2253))
- [ ] Search by mangled (linker) name

## Module Navigation

- [x] `import module_name` → jump to module interface unit ([clangd#2310](https://github.com/clangd/clangd/issues/2310))

  ```cpp
  import mylib;  // go-to-def on mylib → module interface unit (export module mylib;)
  ```

- [ ] `import :partition` → jump to partition unit

  ```cpp
  import :core;  // go-to-def on core → partition unit (export module mylib:core;)
  ```

- [ ] Navigate between interface and implementation units of the same module (implementation → interface works via go-to-def on the `module m;` name; the reverse is not implemented)

  ```cpp
  // interface unit:       export module mylib;
  // implementation unit:  module mylib;
  // go-to-def on mylib → switch between interface and implementation units
  ```

- [ ] Dot-separated module name — navigate each segment to its module

  ```cpp
  import std.io;
  // go-to-def on io → std.io module interface
  // go-to-def on std → std module interface (if exists)
  ```

## Document Highlight

Highlight all references to the symbol under cursor within the current file (`textDocument/documentHighlight`).

- [ ] Highlight all references to the symbol under cursor in the current file
- [ ] Read/Write classification for symbol highlights
- [ ] Control flow token highlighting (`return`, `break`, `continue`, `throw`, `case`/`default` for switch) ([clangd#1921](https://github.com/clangd/clangd/issues/1921))

  ```cpp
  for (int i = 0; i < n; i++) {
      for (int j = 0; j < m; j++) {
          if (done) break;     // highlighting break → also highlights inner for
          if (skip) continue;  // highlighting continue → also highlights inner for
      }
  }
  ```

## Switch Source/Header

- [ ] Switch between source and header file

## Changelog

| Date | Change                                                                                             | PR  |
| ---- | -------------------------------------------------------------------------------------------------- | --- |
| —    | go-to-definition on include directives and module names                                            | —   |
| —    | declaration / implementation / typeDefinition; references includeDeclaration includes declarations | —   |
| —    | Index-based go-to-definition and find references                                                   | —   |
| —    | Call hierarchy (incoming/outgoing)                                                                 | —   |
| —    | Type hierarchy (supertypes/subtypes)                                                               | —   |
