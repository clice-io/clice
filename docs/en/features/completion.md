# Code Completion

## Include Path Completion

Triggered by `<`, `"`, `/` characters. Handled before AST (preamble-level, no compilation needed). Quoted completion searches the configured include directories, not the includer's own directory (unless it is on the include path).

<!-- BEGIN GENERATED ITEMS: include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Quoted include paths**

Headers and directories from the configured search path, directories marked by a trailing slash

Answered by the server before any compilation, so only the server path
exists for this fixture.

```snap-code_completion
feature: code_completion
code: |
  #include "snap§(pos)"
snapshot: |
  pos:
  - { label: "snap_dir/", kind: File }
  - { label: "snap_header.h", kind: File }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Angled include paths**

The same search-path candidates in the angled form

```snap-code_completion
feature: code_completion
code: |
  #include <snap§(pos)>
snapshot: |
  pos:
  - { label: "snap_dir/", kind: File }
  - { label: "snap_header.h", kind: File }
```

<!-- END CAPABILITY -->

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

<!-- BEGIN GENERATED ITEMS: module_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Import statements**

Known module names complete after `import`, with the closing semicolon inserted

Answered by the server from its module map, so only the server path
exists for this fixture; the sibling module interface is opened first
so the module is known. The statement stays unterminated — a `;` on
the line means the import is already complete and nothing is offered.

```snap-code_completion
feature: code_completion
code: |
  import ma§(pos)
file mod_math.cppm: |
  export module math;

  export int add(int a, int b) {
      return a + b;
  }
snapshot: |
  pos:
  - { label: "math", kind: Module, insert: "math;" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] Trigger on space character ([#460](https://github.com/clice-io/clice/pull/460))

  Two-layer gating avoids firing on every space keystroke: the server
  registers ` ` (space) as a trigger character, and space-triggered requests
  only proceed in import contexts (`import `, `export import `); all other
  spaces return empty immediately. This follows the same pattern used by
  TypeScript/Haxe language extensions ([vscode#67714](https://github.com/microsoft/vscode/issues/67714)).

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

<!-- BEGIN GENERATED ITEMS: member_access -->

<!-- BEGIN CAPABILITY: supported -->

**Members of a class**

fields, methods, the destructor and operators complete with plain names

The destructor completes as `~Account` (never `~struct Account`),
`operator=` keeps no space before `=`, and a conversion operator
spells its target type.

```snap-code_completion
feature: code_completion
code: |
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
      acc.§(pos)
  }
snapshot: |
  pos:
  - { label: "Account", kind: Class, edit: "22:8-22:8" }
  - { label: "balance", kind: Field, description: "int", edit: "22:8-22:8" }
  - { label: "bazzzz", kind: Method, detail: "(int a, int b)", description: "int", edit: "22:8-22:8" }
  - { label: "operator Wallet", kind: Method, detail: "()", edit: "22:8-22:8" }
  - { label: "operator=", kind: Method, detail: "(…) +2 overloads", edit: "22:8-22:8" }
  - { label: "~Account", kind: Method, detail: "()", description: "void", edit: "22:8-22:8" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Members of an instantiated class template**

The destructor label keeps the written template arguments

```snap-code_completion
feature: code_completion
code: |
  // The member access expression is left dangling at the point.
  template <typename T>
  struct Box {
      T value;
  };

  void bar() {
      Box<int> b;
      b.§(pos)
  }
snapshot: |
  pos:
  - { label: "Box", kind: Class, edit: "13:6-13:6" }
  - { label: "operator=", kind: Method, detail: "(…) +2 overloads", edit: "13:6-13:6" }
  - { label: "value", kind: Field, description: "int", edit: "13:6-13:6" }
  - { label: "~Box<int>", kind: Method, detail: "()", description: "void", edit: "13:6-13:6" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pointer member access**

`->` on a pointer completes the pointee's members

```snap-code_completion
feature: code_completion
code: |
  // The member access expression is left dangling at the point.
  struct Node {
      int value;
      Node* next;
      int compute(int a);
  };

  void bar() {
      Node* p;
      p->§(pos)
  }
snapshot: |
  pos:
  - { label: "Node", kind: Class, edit: "14:7-14:7" }
  - { label: "compute", kind: Method, detail: "(int a)", description: "int", edit: "14:7-14:7" }
  - { label: "next", kind: Field, description: "Node *", edit: "14:7-14:7" }
  - { label: "operator=", kind: Method, detail: "(…) +2 overloads", edit: "14:7-14:7" }
  - { label: "value", kind: Field, description: "int", edit: "14:7-14:7" }
  - { label: "~Node", kind: Method, detail: "()", description: "void", edit: "14:7-14:7" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scope-qualified members**

After `::` static data, nested types, methods and the injected class name all list

Qualified completion is not filtered to the statically-reachable subset:
instance fields and the destructor show up alongside the static members
and nested types.

```snap-code_completion
feature: code_completion
code: |
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
      int v = Config::§(pos);
  }
snapshot: |
  pos:
  - { label: "Config", kind: Class, edit: "22:20-22:20" }
  - { label: "Nested", kind: Class, edit: "22:20-22:20" }
  - { label: "instance_field", kind: Field, description: "int", edit: "22:20-22:20" }
  - { label: "make", kind: Method, detail: "(int seed)", description: "int", edit: "22:20-22:20" }
  - { label: "operator=", kind: Method, detail: "(…) +2 overloads", edit: "22:20-22:20" }
  - { label: "shared_count", kind: Variable, description: "int", edit: "22:20-22:20" }
  - { label: "~Config", kind: Method, detail: "()", description: "void", edit: "22:20-22:20" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inherited members**

A derived object completes its own members and those of its base

```snap-code_completion
feature: code_completion
code: |
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
      d.§(pos)
  }
snapshot: |
  pos:
  - { label: "Base", kind: Class, edit: "17:6-17:6" }
  - { label: "Derived", kind: Class, edit: "17:6-17:6" }
  - { label: "base_field", kind: Field, description: "int", edit: "17:6-17:6" }
  - { label: "base_method", kind: Method, detail: "()", description: "int", edit: "17:6-17:6" }
  - { label: "derived_field", kind: Field, description: "int", edit: "17:6-17:6" }
  - { label: "operator=", kind: Method, detail: "(…) +2 overloads", edit: "17:6-17:6" }
  - { label: "~Base", kind: Method, detail: "()", description: "void", edit: "17:6-17:6" }
  - { label: "~Derived", kind: Method, detail: "()", description: "void", edit: "17:6-17:6" }
```

<!-- END CAPABILITY -->

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

<!-- BEGIN GENERATED ITEMS: symbols -->

<!-- BEGIN CAPABILITY: supported -->

**Unqualified lookup with fuzzy prefix matching**

Strong prefix matches survive, weak subsequence matches and unqualified namespace members do not

```snap-code_completion
feature: code_completion
code: |
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
      fo§(pos);
  }
snapshot: |
  pos:
  - { label: "folded", kind: Variable, detail: "(int x)", description: "void", edit: "20:4-20:6" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class template deduplication**

A name that is also constructors and a deduction guide stays a single class entry

```snap-code_completion
feature: code_completion
code: |
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
      Fo§(pos)
  }
snapshot: |
  pos:
  - { label: "Foo", kind: Class, detail: "<typename T>", edit: "19:4-19:6" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructor labels stay plain**

Class template constructors and deduction guides complete as the bare class name, never a templated spelling

```snap-code_completion
feature: code_completion
code: |
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
      Ba§(pos)
  }
snapshot: |
  default:
    pos:
    - { label: "Bazzz", kind: Class, detail: "<typename T, typename U>", edit: "20:4-20:6" }
    - { label: "bar", kind: Function, detail: "()", description: "void", edit: "20:4-20:6" }

  configured:
    pos:
    - { label: "Bazzz", kind: Class, detail: "<typename T, typename U>", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Constructor, detail: "<typename T, typename U>()", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Constructor, detail: "<typename T, typename U>(T x)", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Constructor, detail: "<typename T, typename U>(T x, U y)", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Function, detail: "(Bazzz<T, U>)", description: "Bazzz<T, U>", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Function, detail: "(T x, U y)", description: "Bazzz<T, U>", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Function, detail: "(T)", description: "Bazzz<T, int>", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Function, detail: "<typename T, typename U>()", description: "Bazzz<T, U>", edit: "20:4-20:6" }
    - { label: "Bazzz", kind: Function, detail: "<typename T, typename U>(T x)", description: "Bazzz<T, U>", edit: "20:4-20:6" }
    - { label: "bar", kind: Function, detail: "()", description: "void", edit: "20:4-20:6" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keyword patterns**

Keywords complete like any candidate, with plain insert text

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  int x = tru§(pos)
snapshot: |
  pos:
  - { label: "true", kind: Snippet, edit: "6:8-6:11" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macros**

object-like macros complete as constants, function-like ones as functions with a parameter signature; argument snippets follow the function setting

```snap-code_completion
feature: code_completion
code: |
  #define RETRY_LIMIT 3

  #define CLAMP(value, limit) ((value) < (limit) ? (value) : (limit))

  int a = RETRY§(object);
  int b = CLA§(function);
snapshot: |
  default:
    function:
    - { label: "CLAMP", kind: Function, detail: "(value, limit)", edit: "11:8-11:11" }
    - { label: "class", kind: Keyword, edit: "11:8-11:11" }

    object:
    - { label: "RETRY_LIMIT", kind: Constant, edit: "10:8-10:13" }

  configured:
    function:
    - { label: "CLAMP", kind: Function, detail: "(value, limit)", edit: "11:8-11:11", insert: "CLAMP(${1:value}, ${2:limit})", snippet: true }
    - { label: "class", kind: Keyword, edit: "11:8-11:11" }

    object:
    - { label: "RETRY_LIMIT", kind: Constant, edit: "10:8-10:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro shadowing a declaration**

A name redefined as a macro completes as the macro, not the shadowed declaration

```snap-code_completion
feature: code_completion
code: |
  void GUARD(int);
  #define GUARD 1

  int BOUND(int lo, int hi);
  #define BOUND(lo, hi) ((lo) < (hi) ? (lo) : (hi))

  int a = GUAR§(object);
  int b = BOUN§(function);
snapshot: |
  function:
  - { label: "BOUND", kind: Function, detail: "(lo, hi)", edit: "12:8-12:12" }

  object:
  - { label: "GUARD", kind: Constant, edit: "11:8-11:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Completion inside macro arguments**

Member access written as a macro argument completes as it would outside the macro

```snap-code_completion
feature: code_completion
code: |
  #define WRAP(...) __VA_ARGS__

  struct Config {
      int retries;
      int timeout;
  };

  void run() {
      Config config;
      WRAP(config.§(argument));
  }
snapshot: |
  argument:
  - { label: "Config", kind: Class, edit: "14:16-14:16" }
  - { label: "operator=", kind: Method, detail: "(…) +2 overloads", edit: "14:16-14:16" }
  - { label: "retries", kind: Field, description: "int", edit: "14:16-14:16" }
  - { label: "timeout", kind: Field, description: "int", edit: "14:16-14:16" }
  - { label: "~Config", kind: Method, detail: "()", description: "void", edit: "14:16-14:16" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespace-qualified lookup**

`ns::` lists the namespace's own members

```snap-code_completion
feature: code_completion
code: |
  // The qualified-id is left dangling at the point.
  namespace geometry {

  int area_of(int r);

  struct Point {
      int x;
  };

  int origin;

  }  // namespace geometry

  void bar() {
      int v = geometry::§(pos);
  }
snapshot: |
  pos:
  - { label: "Point", kind: Class, edit: "19:22-19:22" }
  - { label: "area_of", kind: Function, detail: "(int r)", description: "int", edit: "19:22-19:22" }
  - { label: "origin", kind: Variable, description: "int", edit: "19:22-19:22" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum members**

A scoped enum lists through `Type::`, an unscoped enumerator completes by bare name

```snap-code_completion
feature: code_completion
code: |
  // Both completion prefixes dangle; the statements stay
  // semicolon-terminated so the second marker is not dragged into recovery.
  enum class Color { Red, Green, Blue };

  enum Fruit { Apple, Banana };

  void bar() {
      Color c = Color::§(scoped);
      int f = App§(unscoped);
  }
snapshot: |
  scoped:
  - { label: "Blue", kind: EnumMember, description: "Color", edit: "12:21-12:21" }
  - { label: "Green", kind: EnumMember, description: "Color", edit: "12:21-12:21" }
  - { label: "Red", kind: EnumMember, description: "Color", edit: "12:21-12:21" }

  unscoped:
  - { label: "Apple", kind: EnumMember, description: "Fruit", edit: "13:12-13:15" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Local shadowing a global**

The shadowed global does not appear as a duplicate entry

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix dangles as an unfinished statement.
  int counter = 0;

  void bar() {
      int counter = 1;
      int v = coun§(pos);
  }
snapshot: |
  pos:
  - { label: "counter", kind: Variable, description: "int", edit: "10:12-10:16" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Using-declaration**

A name pulled in with `using` completes unqualified

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix dangles as an unfinished statement.
  namespace lib {

  int helper_fn(int x);

  }

  using lib::helper_fn;

  void bar() {
      int v = help§(pos);
  }
snapshot: |
  pos:
  - { label: "helper_fn", kind: Function, detail: "(int x)", description: "int", edit: "15:12-15:16" }
```

<!-- END CAPABILITY -->

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

<!-- BEGIN GENERATED ITEMS: functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**Signature and return type details**

The parameter list and return type ride along as label details

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  double foooo(int x, float y);

  int x = fo§(pos)
snapshot: |
  pos:
  - { label: "foooo", kind: Function, detail: "(int x, float y)", description: "double", edit: "8:8-8:10" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overload bundling**

An overload set collapses into one entry with an overload count

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);
  double foooo(double d);

  int x = fooo§(pos)
snapshot: |
  pos:
  - { label: "foooo", kind: Function, detail: "(…) +3 overloads", edit: "10:8-10:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unbundled overloads**

With bundling off, every overload is its own entry with its own signature

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);
  double foooo(double d);

  int x = fooo§(pos)
snapshot: |
  default:
    pos:
    - { label: "foooo", kind: Function, detail: "(…) +3 overloads", edit: "11:8-11:12" }

  configured:
    pos:
    - { label: "foooo", kind: Function, detail: "(double d)", description: "double", edit: "11:8-11:12" }
    - { label: "foooo", kind: Function, detail: "(int x)", description: "int", edit: "11:8-11:12" }
    - { label: "foooo", kind: Function, detail: "(int x, int y)", description: "int", edit: "11:8-11:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter placeholder snippets**

Calls insert tab-stop placeholders per argument; a no-argument function stays plain text

```snap-code_completion
feature: code_completion
code: |
  // The completion prefixes dangle as unfinished statements.
  int foooo(int x, float y);
  void nothing_to_fill();

  struct Foo {
      int bazzzz(int a, int b);
  };

  void bar() {
      Foo f;
      fo§(free_function);
      no§(no_arguments);
      f.ba§(method);
  }
snapshot: |
  default:
    free_function:
    - { label: "Foo", kind: Class, edit: "16:4-16:6" }
    - { label: "foooo", kind: Function, detail: "(int x, float y)", description: "int", edit: "16:4-16:6" }

    method:
    - { label: "bazzzz", kind: Method, detail: "(int a, int b)", description: "int", edit: "18:6-18:8" }

    no_arguments:
    - { label: "noexcept", kind: Snippet, edit: "17:4-17:6" }
    - { label: "nothing_to_fill", kind: Function, detail: "()", description: "void", edit: "17:4-17:6" }

  configured:
    free_function:
    - { label: "Foo", kind: Class, edit: "16:4-16:6" }
    - { label: "Foo", kind: Constructor, detail: "()", edit: "16:4-16:6" }
    - { label: "Foo", kind: Constructor, detail: "(Foo &&)", edit: "16:4-16:6", insert: "Foo(${1:Foo &&})", snippet: true }
    - { label: "Foo", kind: Constructor, detail: "(const Foo &)", edit: "16:4-16:6", insert: "Foo(${1:const Foo &})", snippet: true }
    - { label: "foooo", kind: Function, detail: "(int x, float y)", description: "int", edit: "16:4-16:6", insert: "foooo(${1:int x}, ${2:float y})", snippet: true }

    method:
    - { label: "bazzzz", kind: Method, detail: "(int a, int b)", description: "int", edit: "18:6-18:8", insert: "bazzzz(${1:int a}, ${2:int b})", snippet: true }

    no_arguments:
    - { label: "noexcept", kind: Snippet, edit: "17:4-17:6" }
    - { label: "nothing_to_fill", kind: Function, detail: "()", description: "void", edit: "17:4-17:6" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Snippets defer to bundling**

While overloads are bundled, argument snippets stay off even when enabled

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);

  int z = fo§(pos)
snapshot: |
  default:
    pos:
    - { label: "foooo", kind: Function, detail: "(…) +2 overloads", edit: "10:8-10:10" }

  configured:
    pos:
    - { label: "foooo", kind: Function, detail: "(…) +2 overloads", edit: "10:8-10:10" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default-argument parameters**

A parameter with a default value drops out of the signature detail

The signature detail keeps only the required parameters; the trailing
`int retries = 3` is elided.

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  int configure(int timeout, int retries = 3);

  int x = confi§(pos)
snapshot: |
  pos:
  - { label: "configure", kind: Function, detail: "(int timeout)", description: "int", edit: "11:8-11:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variadic signature**

A trailing `...` shows in the parameter detail

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  int printf_like(const char* fmt, ...);

  int x = printf§(pos)
snapshot: |
  pos:
  - { label: "printf_like", kind: Function, detail: "(const char *fmt, ...)", description: "int", edit: "8:8-8:14" }
```

<!-- END CAPABILITY -->

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

### Filtering & Ranking

<!-- BEGIN GENERATED ITEMS: filtering_ranking -->

<!-- BEGIN CAPABILITY: supported -->

**Underscore filtering**

underscore-prefixed internal symbols hide unless the typed prefix itself starts with one

```snap-code_completion
feature: code_completion
code: |
  // The completion prefixes are undeclared identifiers. The
  // statements stay semicolon-terminated: an unterminated one puts the
  // NEXT marker into a recovery context, which completion drops entirely.
  int _private_thing;
  int public_thing;

  int x = pu§(hidden);
  int y = _p§(typed_underscore);
snapshot: |
  hidden:
  - { label: "public_thing", kind: Variable, description: "int", edit: "11:8-11:10" }

  typed_underscore:
  - { label: "_Pragma", kind: Constant, edit: "12:8-12:10" }
  - { label: "_private_thing", kind: Variable, description: "int", edit: "12:8-12:10" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated tagging**

A [[deprecated]] candidate carries the Deprecated tag, its plain sibling does not

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix cuts the initializer mid-expression.
  [[deprecated]] int old_thing(int x);
  int new_thing(int x);

  int z = thing§(pos)
snapshot: |
  pos:
  - { label: "new_thing", kind: Function, detail: "(int x)", description: "int", edit: "9:8-9:13" }
  - { label: "old_thing", kind: Function, detail: "(int x)", description: "int", edit: "9:8-9:13", deprecated: true }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Word-boundary fuzzy match**

Prefix `fb` matches the word starts of `foo_bar_baz`

`frobnicate` is only a weak scattered subsequence of `fb` and is dropped;
`foo_bar_baz` matches on the `foo`/`bar` word boundaries and survives.

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix dangles as an unfinished statement.
  int foo_bar_baz;
  int frobnicate;

  void bar() {
      int v = fb§(pos);
  }
snapshot: |
  pos:
  - { label: "foo_bar_baz", kind: Variable, description: "int", edit: "13:12-13:14" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Case-insensitive prefix**

A lowercase prefix matches a mixed-case identifier

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix dangles as an unfinished statement.
  int MyLongName;

  void bar() {
      int v = mylong§(pos);
  }
snapshot: |
  pos:
  - { label: "MyLongName", kind: Variable, description: "int", edit: "9:12-9:18" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Prefix outranks subsequence**

An exact-prefix candidate sorts above a scattered subsequence match

For prefix `fo`, `format_output` is a true prefix and outscores
`fast_math_operation`, which only matches as a subsequence.

```snap-code_completion
feature: code_completion
code: |
  // The completion prefix dangles as an unfinished statement.
  int format_output;
  int fast_math_operation;

  void bar() {
      int v = fo§(pos);
  }
snapshot: |
  pos:
  - { label: "format_output", kind: Variable, description: "int", edit: "13:12-13:14" }
  - { label: "fast_math_operation", kind: Variable, description: "int", edit: "13:12-13:14" }
```

<!-- END CAPABILITY -->

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
- [ ] Macro definitions as documentation ([clangd#1485](https://github.com/clangd/clangd/issues/1485))

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
