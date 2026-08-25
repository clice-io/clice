# Code Completion

## Include Path Completion

Triggered by `<`, `"`, `/` characters. Handled before AST (preamble-level, no compilation needed). Quoted completion searches the configured include directories, not the includer's own directory (unless it is on the include path).

<!-- BEGIN GENERATED ITEMS: Include Path Completion -->

- [x] Quoted include paths — headers and directories from the configured search path, directories marked by a trailing slash

  Answered by the server before any compilation, so only the server path
  exists for this fixture.

  <details>
  <summary>Example</summary>

  ```cpp
  #include "snap"
  ```

  </details>

- [x] Angled include paths — the same search-path candidates in the angled form

  <details>
  <summary>Example</summary>

  ```cpp
  #include <snap>
  ```

  </details>

<!-- END GENERATED ITEMS -->

**Trigger contexts**

- [ ] `#include_next` — must detect that the directive is `#include_next`, not `#include`, and adjust search to start from the directory _after_ the one that provided the current file

  ```cpp
  // in <bits/stl_vector.h>, provided by /usr/include/c++/14/
  #include_next <^>  // search starts AFTER /usr/include/c++/14/, skipping it
  ```

- [ ] `__has_include()` / `__has_embed()` — trigger include path completion inside these constructs

  ```cpp
  #if __has_include(<^>)  // suggest headers, same as #include <
  ```

- [ ] `#embed` directive completion

  ```cpp
  #embed <^>  // suggest embeddable resource files
  ```

**Candidates and ranking**

- [x] Traverse compiler search paths from compilation database
- [x] Both files and directories are candidates; directories are distinguished by a trailing `/` in the label
- [ ] Filter out already-included headers

  ```cpp
  #include <vector>
  #include <^>  // should not suggest "vector" again
  ```

- [ ] Deprioritize private/internal headers — paths that normal users should not include directly:
  - Single `_` prefix: lower priority (e.g. `_ctype.h`)
  - Double `__` prefix: even lower priority (compiler built-in internals like `__config`, `__bit_reference`)
  - Keywords like `detail`, `internal`, `impl`, `bits` in the path (third-party library private headers like `boost/detail/`, `bits/stdc++.h`)

  ```cpp
  #include <^>        // __config, _ctype.h, bits/stdc++.h rank near bottom
  #include <boost/^>  // boost/detail/ ranks lower than boost/asio/
  ```

- [ ] Path-distance-based ranking: headers closer to the current file in the project tree rank higher

**Insertion behavior**

- [ ] Directory completion should NOT insert the trailing `/` — let the user type it to re-trigger completion for the next level (currently the `/` is baked into the inserted text, which prevents the editor from auto-triggering the next completion round) ([clangd#395](https://github.com/clangd/clangd/issues/395))

  ```cpp
  #include <sys^>  // accept "sys" → inserts "sys", user types "/" → next completion fires
  ```

## Module Completion

Detected via text context analysis. Handled before AST (preamble-level, no compilation needed).

### Import

Triggered when cursor is after `import` or `export import`.

<!-- BEGIN GENERATED ITEMS: Module Completion -->

- [x] Import statements — known module names complete after `import`, with the closing semicolon inserted

  Answered by the server from its module map, so only the server path
  exists for this fixture; the sibling module interface is opened first
  so the module is known. The statement stays unterminated — a `;` on
  the line means the import is already complete and nothing is offered.

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  import ma
  ```

  `mod_math.cppm`:

  ```cpp
  export module math;

  export int add(int a, int b) {
      return a + b;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [ ] Trigger on space character ([#460](https://github.com/clice-io/clice/pull/460))

  Requires two-layer gating to avoid firing on every space keystroke:
  1. **Server-side**: register ` ` (space) as a trigger character so the client sends completion requests on space.
  2. **Extension-side middleware**: intercept space-triggered requests and only forward them when the current line matches `import ` or `export import ` (cheap string check, zero IPC overhead for non-import spaces). All other spaces return empty immediately.

  This follows the same pattern used by TypeScript/Haxe language extensions ([vscode#67714](https://github.com/microsoft/vscode/issues/67714)).

- [ ] Exclude self-module from results (self-import is invalid) — **FIXME**
- [ ] Partition import within the same module

  ```cpp
  // inside module foo
  import :^  // suggest :core, :io (only foo's own partitions)
  ```

  Note: `import M:part;` is not valid C++ — partitions can only be imported via the short form `import :part;` from within the same module.

- [ ] Hierarchical dot-completion

  ```cpp
  import std.^  // suggest io, compat, etc.
  ```

  Note: dots in module names are a naming convention, not language-level hierarchy, but dot-triggered completion is still valuable UX.

- [ ] Filter out non-exported (internal) partitions of other modules
- [ ] Header unit import

  ```cpp
  import <^>  // suggest importable headers (same candidates as #include)
  import "^"  // same, quoted form
  ```

- [ ] Auto-insert `import` statement on symbol completion (like auto-include for headers)

  ```cpp
  std::vector^  // on accept, also insert "import std;" at the top
  ```

### Declaration

Completion within module declaration contexts (`module` / `export module`).

- [ ] `import` / `module` keyword completion

  ```cpp
  imp^  // suggest "import" keyword
  mod^  // suggest "module" keyword
  ```

- [ ] Module name completion after `module` / `export module`

  ```cpp
  module my^  // suggest existing module names (useful when writing implementation units)
  ```

- [ ] Partition name completion after `:`

  ```cpp
  export module mylib:^  // suggest existing partition names of mylib
  module mylib:^  // same, for partition implementation unit
  ```

- [ ] `module :private;` completion (private module fragment)

  ```cpp
  module :^  // suggest "private"
  ```

- [ ] `export import :partition` re-export completion in primary interface unit

  ```cpp
  // in primary interface unit of mylib
  export import :^  // suggest mylib's interface partitions that need re-exporting
  ```

## Semantic Code Completion

Triggered by `.`, `->`, `::`, or quickSuggestions. Forwarded to Clang `CodeCompleteConsumer` via stateless worker.

### Member Access

<!-- BEGIN GENERATED ITEMS: Member Access -->

- [x] Members of a class — fields, methods, the destructor and operators complete with plain names

  The destructor completes as `~Account` (never `~struct Account`),
  `operator=` keeps no space before `=`, and a conversion operator
  spells its target type.

  <details>
  <summary>Example</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  struct Wallet {
      int cents;
  };

  struct Account {
      int balance;
      int bazzzz(int a, int b);
      operator Wallet();
  };

  void bar() {
      Account acc;
      acc.
  }
  ```

  </details>

- [x] Members of an instantiated class template — the destructor label keeps the written template arguments

  <details>
  <summary>Example</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  template <typename T>
  struct Box {
      T value;
  };

  void bar() {
      Box<int> b;
      b.
  }
  ```

  </details>

- [x] Pointer member access — `->` on a pointer completes the pointee's members

  <details>
  <summary>Example</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  struct Node {
      int value;
      Node* next;
      int compute(int a);
  };

  void bar() {
      Node* p;
      p->
  }
  ```

  </details>

- [x] Scope-qualified members — after `::` static data, nested types, methods and the injected class name all list

  Qualified completion is not filtered to the statically-reachable subset:
  instance fields and the destructor show up alongside the static members
  and nested types.

  <details>
  <summary>Example</summary>

  ```cpp
  // The qualified-id is left dangling at the point.
  struct Config {
      static int shared_count;
      static int make(int seed);

      struct Nested {
          int a;
      };

      int instance_field;
  };

  void bar() {
      int v = Config::;
  }
  ```

  </details>

- [x] Inherited members — a derived object completes its own members and those of its base

  <details>
  <summary>Example</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  struct Base {
      int base_field;
      int base_method();
  };

  struct Derived : Base {
      int derived_field;
  };

  void bar() {
      Derived d;
      d.
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] `->` — pointer member access (with Clang fixup)
- [x] `::` — namespace/class scope members
- [ ] Dot-to-arrow: typing `.` on a pointer triggers `->` member completion with automatic replacement ([clangd#1349](https://github.com/clangd/clangd/issues/1349))

  ```cpp
  std::unique_ptr<Foo> ptr;
  ptr.^  // suggest Foo's members, insert as ptr->bar()
  ```

- [ ] Show free functions whose first parameter matches the object type alongside member results

  ```cpp
  std::vector<int> v;
  v.^  // also suggest std::sort(v, ...), std::find(v, ...) etc.
  ```

- [ ] `operator[]`, `operator->`, `operator()` in member suggestions
- [ ] Prioritize direct members for the operator typed (`.` members for `.`, `->` members for `->`)

### Designated Initializers

- [ ] Sort completions in declaration order (required by C++20 designated initializers) ([clangd#965](https://github.com/clangd/clangd/issues/965))

  ```cpp
  struct Cfg { int width; int height; bool fullscreen; };
  Cfg c = { .^  // suggest: .width, .height, .fullscreen (in this order)
  ```

- [ ] Filter out already-used designators

  ```cpp
  Cfg c = { .width = 800, .^  // only suggest .height, .fullscreen
  ```

- [ ] Compound literal designated initializers (`(struct T){ .field = }`)
- [ ] Anonymous struct/union member designators

  ```cpp
  struct S { union { int i; float f; }; };
  S s = { .^  // suggest .i, .f
  ```

- [ ] "Fill all members" snippet

  ```cpp
  Cfg c = { ^  // first item: .width = ${1}, .height = ${2}, .fullscreen = ${3}
  ```

### Override & Out-of-line Definition

- [ ] Virtual function override completion with full signature and `override` keyword

  ```cpp
  struct Base { virtual void draw(int x, int y) const; };
  struct Derived : Base {
      ^  // suggest: void draw(int x, int y) const override
  };
  ```

- [ ] Full inheritance hierarchy traversal for override candidates ([clangd#226](https://github.com/clangd/clangd/issues/226), [clangd#2374](https://github.com/clangd/clangd/issues/2374))

  ```cpp
  struct A { virtual void f(); };
  struct B : A { };
  struct C : B {
      ^  // suggest: void f() override (from A, through B)
  };
  ```

- [ ] Out-of-line definition completion

  ```cpp
  // in .cpp file
  void MyClass::^  // suggest all member functions with full signature + body snippet
  ```

- [ ] Show all members (including private/protected) in definition contexts

  ```cpp
  class Foo { private: void secret(); };
  void Foo::^  // must include "secret" — this is a definition, not a call
  ```

- [ ] Constructors after `::` in definition contexts
- [ ] Suppress redundant template parameters for constructors/destructors in class templates

  ```cpp
  template<typename T>
  struct Vec { Vec(); ~Vec(); };

  template<typename T>
  Vec<T>::^  // suggest "Vec()" and "~Vec()", not "Vec<T>()" or "~Vec<T>()"
  ```

### Symbols

<!-- BEGIN GENERATED ITEMS: Symbols -->

- [x] Unqualified lookup with fuzzy prefix matching — strong prefix matches survive, weak subsequence matches and unqualified namespace members do not

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion expression dangles as an unfinished statement.
  namespace A {

  void fooooo();

  }

  struct X {
      void operator()() {}
  };

  void bar() {
      X functor;
      auto folded = [](int x) {
      };
      fo;
  }
  ```

  </details>

- [x] Class template deduplication — a name that is also constructors and a deduction guide stays a single class entry

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  template <typename T>
  struct Foo {
      Foo() {}

      Foo(T x) {}

      Foo(T x, T y) {}
  };

  template <typename T>
  Foo(T) -> Foo<T>;

  void bar() {
      Fo
  }
  ```

  </details>

- [x] Constructor labels stay plain — class template constructors and deduction guides complete as the bare class name, never a templated spelling

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  template <typename T, typename U>
  struct Bazzz {
      Bazzz() {}

      Bazzz(T x) {}

      Bazzz(T x, U y) {}
  };

  template <typename T>
  Bazzz(T) -> Bazzz<T, int>;

  void bar() {
      Ba
  }
  ```

  </details>

- [x] Keyword patterns — keywords complete like any candidate, with plain insert text

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int x = tru
  ```

  </details>

- [x] Macros — object-like macros complete as constants, function-like ones as functions with a parameter signature; argument snippets follow the function setting

  <details>
  <summary>Example</summary>

  ```cpp
  #define RETRY_LIMIT 3

  #define CLAMP(value, limit) ((value) < (limit) ? (value) : (limit))

  int a = RETRY;
  int b = CLA;
  ```

  </details>

- [x] Macro shadowing a declaration — a name redefined as a macro completes as the macro, not the shadowed declaration

  <details>
  <summary>Example</summary>

  ```cpp
  void GUARD(int);
  #define GUARD 1

  int BOUND(int lo, int hi);
  #define BOUND(lo, hi) ((lo) < (hi) ? (lo) : (hi))

  int a = GUAR;
  int b = BOUN;
  ```

  </details>

- [x] Namespace-qualified lookup — `ns::` lists the namespace's own members

  <details>
  <summary>Example</summary>

  ```cpp
  // The qualified-id is left dangling at the point.
  namespace geometry {

  int area_of(int r);

  struct Point {
      int x;
  };

  int origin;

  }  // namespace geometry

  void bar() {
      int v = geometry::;
  }
  ```

  </details>

- [x] Enum members — a scoped enum lists through `Type::`, an unscoped enumerator completes by bare name

  <details>
  <summary>Example</summary>

  ```cpp
  // Both completion prefixes dangle; the statements stay
  // semicolon-terminated so the second marker is not dragged into recovery.
  enum class Color { Red, Green, Blue };

  enum Fruit { Apple, Banana };

  void bar() {
      Color c = Color::;
      int f = App;
  }
  ```

  </details>

- [x] Local shadowing a global — the shadowed global does not appear as a duplicate entry

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int counter = 0;

  void bar() {
      int counter = 1;
      int v = coun;
  }
  ```

  </details>

- [x] Using-declaration — a name pulled in with `using` completes unqualified

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  namespace lib {

  int helper_fn(int x);

  }

  using lib::helper_fn;

  void bar() {
      int v = help;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] Qualified name lookup (`std::`)
- [x] Argument-dependent lookup (ADL) candidates
- [x] Macro completion — object-like and function-like macros in the candidate set
- [ ] Snippet patterns with placeholders (function bodies, control flow)
- [ ] C++ attribute completion

  ```cpp
  [[^]]  // suggest: nodiscard, deprecated, maybe_unused, likely, ...
  ```

- [ ] Cross-scope completion including class/struct-scoped symbols (inner types, static methods)

  ```cpp
  struct Outer { struct Inner {}; static int count; };
  Inn^  // suggest Outer::Inner from a different scope
  ```

- [ ] Respect namespace aliases in inserted qualifiers (prefer shortest valid qualifier)

  ```cpp
  namespace fs = std::filesystem;
  fs::ex^  // insert "fs::exists", not "std::filesystem::exists"
  ```

- [ ] Language-aware filtering (no C++ symbols in C files in mixed projects)
- [ ] Function-argument comment completion (`/*param=*/` style parameter hints)
- [ ] Identifier-based fallback completion when semantic analysis is unavailable

### Functions & Snippets

All options below live in the `[code_completion]` configuration section.

<!-- BEGIN GENERATED ITEMS: Functions & Snippets -->

- [x] Signature and return type details — the parameter list and return type ride along as label details

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  double foooo(int x, float y);

  int x = fo
  ```

  </details>

- [x] Overload bundling — an overload set collapses into one entry with an overload count

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);
  double foooo(double d);

  int x = fooo
  ```

  </details>

- [x] Unbundled overloads — with bundling off, every overload is its own entry with its own signature

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);
  double foooo(double d);

  int x = fooo
  ```

  </details>

- [x] Parameter placeholder snippets — calls insert tab-stop placeholders per argument; a no-argument function stays plain text

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefixes dangle as unfinished statements.
  int foooo(int x, float y);
  void nothing_to_fill();

  struct Foo {
      int bazzzz(int a, int b);
  };

  void bar() {
      Foo f;
      fo;
      no;
      f.ba;
  }
  ```

  </details>

- [x] Snippets defer to bundling — while overloads are bundled, argument snippets stay off even when enabled

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);

  int z = fo
  ```

  </details>

- [x] Default-argument parameters — a parameter with a default value drops out of the signature detail

  The signature detail keeps only the required parameters; the trailing
  `int retries = 3` is elided.

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int configure(int timeout, int retries = 3);

  int x = confi
  ```

  </details>

- [x] Variadic signature — a trailing `...` shows in the parameter detail

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int printf_like(const char* fmt, ...);

  int x = printf
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [ ] Template argument placeholders (`enable_template_arguments_snippet`)
- [ ] Auto-insert parentheses (`insert_paren_in_function_call`)
- [ ] Look-ahead for existing parentheses/brackets to avoid duplicate insertion

  ```cpp
  foo^(10, 20);  // should NOT insert another pair of parens → foo(10, 20)
  ```

- [ ] Context-sensitive snippet: insert name only (no call syntax) in function pointer contexts

  ```cpp
  void (*fp)(int) = my_fun^;  // insert "my_func", not "my_func(${1:int x})"
  ```

- [ ] Strip C++23 explicit object parameter from signatures and snippets

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show signature "(int x)", not "(this S& self, int x)"
  ```

- [ ] Show default parameter values in signatures ([clangd#100](https://github.com/clangd/clangd/issues/100))

  ```cpp
  void open(std::string path, int mode = 0644);
  open(^  // detail shows "(string path, int mode = 0644)"
  ```

- [ ] Resolve lambda types to actual signatures

  ```cpp
  auto cmp = [](int a, int b) -> bool { return a < b; };
  cmp^  // show "(int a, int b) -> bool", not "<lambda>"
  ```

- [ ] Resolve forwarding function parameters ([clangd#447](https://github.com/clangd/clangd/issues/447))

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(^  // show "(int w, int h)"
  ```

- [ ] `InsertReplaceEdit` support (provide both insert and replace ranges for mid-word completion)

  ```cpp
  refact^orize  // insert: "refactoring^orize", replace: "refactoring"
  ```

- [ ] Set `InsertTextFormat::PlainText` when no placeholders are present

### Templates & Concepts

- [ ] Concept-aware completion: infer available members from concept constraints on template parameters ([clangd#1103](https://github.com/clangd/clangd/issues/1103))

  ```cpp
  template<typename T>
  concept Drawable = requires(T t) { t.draw(); t.resize(int{}, int{}); };

  template<Drawable T>
  void render(T& widget) {
      widget.^  // suggest draw(), resize() from Drawable concept
  }
  ```

- [ ] Dependent type member completion in uninstantiated templates

  ```cpp
  template<typename T>
  void process(std::vector<std::vector<T>>& matrix) {
      matrix[0].^  // resolve operator[] → vector<T>&, suggest push_back(), size() etc.
  }
  ```

- [ ] Use single-instantiation information for generic lambda completion — when a generic lambda is only called from one site, use that site's argument types to provide completion inside the lambda body

  ```cpp
  std::vector<std::string> names;
  std::ranges::sort(names, [](const auto& a, const auto& b) {
      return a.^  // a is deducible as std::string from the single call site
  });
  ```

  ```cpp
  auto results = names | std::views::transform([](const auto& s) {
      return s.^  // s is deducible as std::string
  });
  ```

- [ ] Suppress template parameter snippet for injected class name inside class template body

  ```cpp
  template<typename T>
  struct Vec {
      Vec^  // suggest "Vec", not "Vec<${1:T}>" — injected class name
  };
  ```

### Macros

- [x] Macro name completion, including macros deserialized from the preamble
- [x] Fuzzy matching for macros (same matcher as other symbols)
- [x] Correct `CompletionItemKind`: `Function` for function-like, `Constant` for object-like ([clangd#2002](https://github.com/clangd/clangd/issues/2002))
- [x] Parameter list as the label detail for function-like macros
- [ ] Show macro definition/expansion as documentation ([clangd#1485](https://github.com/clangd/clangd/issues/1485))

  ```cpp
  #define MAX_BUF 4096
  MAX^  // completion detail shows: #define MAX_BUF 4096
  ```

- [x] Parameter placeholders for function-like macros (respect snippet settings)

  ```cpp
  #define CHECK(cond, msg) ...
  CHECK^  // insert: CHECK(${1:cond}, ${2:msg})
  ```

- [ ] Completion inside macro arguments with fallback to enclosing context

  ```cpp
  #define WRAP(...) __VA_ARGS__
  WRAP(some_obj.^)  // should still offer some_obj's members
  ```

### Filtering & Ranking

<!-- BEGIN GENERATED ITEMS: Filtering & Ranking -->

- [x] Underscore filtering — underscore-prefixed internal symbols hide unless the typed prefix itself starts with one

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefixes are undeclared identifiers. The
  // statements stay semicolon-terminated: an unterminated one puts the
  // NEXT marker into a recovery context, which completion drops entirely.
  int _private_thing;
  int public_thing;

  int x = pu;
  int y = _p;
  ```

  </details>

- [x] Deprecated tagging — a [[deprecated]] candidate carries the Deprecated tag, its plain sibling does not

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  [[deprecated]] int old_thing(int x);
  int new_thing(int x);

  int z = thing
  ```

  </details>

- [x] Word-boundary fuzzy match — prefix `fb` matches the word starts of `foo_bar_baz`

  `frobnicate` is only a weak scattered subsequence of `fb` and is dropped;
  `foo_bar_baz` matches on the `foo`/`bar` word boundaries and survives.

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int foo_bar_baz;
  int frobnicate;

  void bar() {
      int v = fb;
  }
  ```

  </details>

- [x] Case-insensitive prefix — a lowercase prefix matches a mixed-case identifier

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int MyLongName;

  void bar() {
      int v = mylong;
  }
  ```

  </details>

- [x] Prefix outranks subsequence — an exact-prefix candidate sorts above a scattered subsequence match

  For prefix `fo`, `format_output` is a true prefix and outscores
  `fast_math_operation`, which only matches as a subsequence.

  <details>
  <summary>Example</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int format_output;
  int fast_math_operation;

  void bar() {
      int v = fo;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] Fuzzy matching with word-boundary-aware scoring (camelCase, snake_case)
- [x] Filter out recovery context results (`CCC_Recovery`)
- [ ] Result limit (`CodeCompletionOptions.limit`)
- [ ] Frecency/recently-used boosting
- [ ] Treat digit-letter boundaries as word breaks ([clangd#1236](https://github.com/clangd/clangd/issues/1236))

  ```cpp
  i32^  // should match int32_t (digit-letter boundary: "32" → "t")
  ```

- [ ] Scope-aware relevance tiers: locals > members > namespace-scope > cross-scope
- [ ] Context-based type boosting (suggest matching enum members when expected type is an enum) ([clangd#462](https://github.com/clangd/clangd/issues/462))

  ```cpp
  enum Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // boost Red, Green, Blue to top
  ```

- [ ] Filter already-used enum values in switch statements

  ```cpp
  switch (color) {
      case Red: break;
      case ^  // suggest Green, Blue only — Red already used
  ```

- [ ] Rank `nullptr` above `NULL` in C++ mode
- [ ] Naming signal boosting

  ```cpp
  auto foo = get^;  // boost getFoo() over getBar()
  ```

- [ ] Reference-count and file-proximity ranking signals
- [ ] Machine-learned ranking model

## Auto-Include Insertion

Not yet implemented. Completing a symbol does not insert `#include` directives.

- [ ] Insert `#include` for unresolved symbols on completion accept

  ```cpp
  std::vec^  // on accept "vector", also insert #include <vector> at top of file
  ```

- [ ] Check transitive include graph to avoid duplicate includes

  ```cpp
  // <algorithm> already includes <iterator> transitively
  std::back_inserter^  // do NOT insert #include <iterator> again
  ```

- [ ] Context-aware: no include insertion for forward declarations or pointer/reference-only usage ([clangd#639](https://github.com/clangd/clangd/issues/639))

  ```cpp
  class Foo;
  Foo*^  // no include needed — forward declaration suffices for pointer
  ```

- [ ] Insert C headers in C files, C++ headers in C++ files

  ```c
  // in a .c file
  size_^  // insert #include <stddef.h>, not #include <cstddef>
  ```

- [ ] Configurable behavior: `always` / `iwyu-only` / `never`
- [ ] Prefer project-relative paths over absolute paths
- [ ] Respect IWYU pragmas and header mappings
- [ ] Auto-insert `import` for C++20 module symbols

## Documentation in Completions

Not yet implemented. Completion items do not include documentation.

- [ ] Extract doc comments from declarations and definitions

  ```cpp
  /// @brief Opens a file at the given path.
  /// @param path The file system path.
  void open(std::string path);

  op^  // completion popup shows the @brief doc
  ```

- [ ] Available regardless of where the definition lives (header, source, index)
- [ ] Propagate template pattern documentation to instantiations
- [ ] Standard library documentation integration

## Trigger Characters

Registered: `. < > : " / *`. Space (` `) is planned but not yet merged ([#460](https://github.com/clice-io/clice/pull/460)).

| Character | Context         | Behavior                                                                                                  |
| --------- | --------------- | --------------------------------------------------------------------------------------------------------- |
| `.`       | Member access   | Semantic completion                                                                                       |
| `->`      | Pointer member  | `[ ]` Not yet working — dot-to-arrow fix-its not propagated                                               |
| `::`      | Via `:` trigger | Scope completion                                                                                          |
| `<`       | `#include <`    | Include path completion                                                                                   |
| `>`       | Template close  | Semantic completion                                                                                       |
| `"`       | `#include "`    | Include path completion                                                                                   |
| `/`       | Path separator  | Include path continuation                                                                                 |
| `*`       | Pointer deref   | Semantic completion                                                                                       |
| ` `       | After `import`  | Module name completion (extension-gated) — **pending [#460](https://github.com/clice-io/clice/pull/460)** |

## LSP Protocol Features

- [ ] `completionItem/resolve` for lazy-loading documentation and details
- [ ] `CompletionList.isIncomplete` flag for incremental filtering
- [ ] `commitCharacters` for auto-accepting completions on specific keystrokes
- [ ] `filterText` / `sortText` for client-side re-filtering

## Changelog

| Date       | Change                                                                | PR                                                 |
| ---------- | --------------------------------------------------------------------- | -------------------------------------------------- |
| 2026-04-06 | `#include` path completion and module import completion (flat prefix) | [#394](https://github.com/clice-io/clice/pull/394) |
| 2024-12-01 | Initial semantic completion                                           | —                                                  |
