# Code Completion

## Include paths

Triggered by `<`, `"`, `/` characters. Handled before AST (preamble-level, no compilation needed). Quoted completion searches the configured include directories, not the includer's own directory (unless it is on the include path).

<!-- BEGIN GENERATED ITEMS: include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Quoted include paths**

Headers and directories from the configured search path, directories marked by a trailing slash

```snap
tests/snap/code_completion/include_path_completion/01_include_quoted.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Angled include paths**

The same search-path candidates in the angled form

```snap
tests/snap/code_completion/include_path_completion/02_include_angled.cpp
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

## Module import

Detected via text context analysis. Handled before AST (preamble-level, no compilation needed).

Triggered when cursor is after `import` or `export import`.

<!-- BEGIN GENERATED ITEMS: module_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Import statements**

Known module names complete after `import`, with the closing semicolon inserted

A statement that already contains its closing semicolon is complete and
offers no module names.

```snap
tests/snap/code_completion/module_completion/01_import_modules/main.cpp
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

## Module declarations

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

## Member access

Triggered by `.`, `->`, `::`, or quickSuggestions. Forwarded to Clang `CodeCompleteConsumer` via stateless worker.

<!-- BEGIN GENERATED ITEMS: member_access -->

<!-- BEGIN CAPABILITY: supported -->

**Members of a class**

Fields, methods, the destructor and operators complete with plain names

The destructor completes as `~Account` (never `~struct Account`),
`operator=` keeps no space before `=`, and a conversion operator
spells its target type.

```snap
tests/snap/code_completion/member_access/01_member_access.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Instantiated class template members**

The destructor label keeps the written template arguments

```snap
tests/snap/code_completion/member_access/02_member_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pointer member access**

`->` on a pointer completes the pointee's members

```snap
tests/snap/code_completion/member_access/03_pointer_arrow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scope-qualified members**

After `::` static data, nested types, methods and the injected class name all list

Qualified completion is not filtered to the statically-reachable subset:
instance fields and the destructor show up alongside the static members
and nested types.

```snap
tests/snap/code_completion/member_access/04_scope_access.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inherited members**

A derived object completes its own members and those of its base

```snap
tests/snap/code_completion/member_access/05_inherited_members.cpp
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

## Designated initializers

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

## Override and out-of-line definitions

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

## Symbols

<!-- BEGIN GENERATED ITEMS: symbols -->

<!-- BEGIN CAPABILITY: supported -->

**Fuzzy unqualified lookup**

Strong prefix matches survive, weak subsequence matches and unqualified namespace members do not

```snap
tests/snap/code_completion/symbols/01_unqualified_lookup.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class template deduplication**

A name that is also constructors and a deduction guide stays a single class entry

```snap
tests/snap/code_completion/symbols/02_template_dedup.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructor labels stay plain**

Class template constructors and deduction guides complete as the bare class name, never a templated spelling

```snap
tests/snap/code_completion/symbols/03_constructor_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keyword patterns**

Keywords complete like any candidate, with plain insert text

```snap
tests/snap/code_completion/symbols/04_pattern_keyword.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macros**

Object-like macros complete as constants, function-like ones as functions with a parameter signature; argument snippets follow the function setting

```snap
tests/snap/code_completion/symbols/05_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro shadowing a declaration**

A name redefined as a macro completes as the macro, not the shadowed declaration

```snap
tests/snap/code_completion/symbols/06_macro_shadow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Completion inside macro arguments**

Member access written as a macro argument completes as it would outside the macro

```snap
tests/snap/code_completion/symbols/07_macro_argument.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespace-qualified lookup**

`ns::` lists the namespace's own members

```snap
tests/snap/code_completion/symbols/08_namespace_qualified.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum members**

A scoped enum lists through `Type::`, an unscoped enumerator completes by bare name

```snap
tests/snap/code_completion/symbols/09_enum_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Local shadowing a global**

The shadowed global does not appear as a duplicate entry

```snap
tests/snap/code_completion/symbols/10_local_shadow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Using-declaration**

A name pulled in with `using` completes unqualified

```snap
tests/snap/code_completion/symbols/11_using_declaration.cpp
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

## Functions and snippets

All options below live in the `[code_completion]` configuration section.

<!-- BEGIN GENERATED ITEMS: functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**Signature and return type details**

The parameter list and return type ride along as label details

```snap
tests/snap/code_completion/functions_snippets/01_function_candidates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overload bundling**

An overload set collapses into one entry with an overload count

```snap
tests/snap/code_completion/functions_snippets/02_overload_bundle.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unbundled overloads**

With bundling off, every overload is its own entry with its own signature

```snap
tests/snap/code_completion/functions_snippets/03_no_bundle_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter placeholder snippets**

Calls insert tab-stop placeholders per argument; a no-argument function stays plain text

```snap
tests/snap/code_completion/functions_snippets/04_snippet_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Snippets defer to bundling**

While overloads are bundled, argument snippets stay off even when enabled

```snap
tests/snap/code_completion/functions_snippets/05_snippet_bundle_mode.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default-argument parameters**

A parameter with a default value drops out of the signature detail

The signature detail keeps only the required parameters; the trailing
`int retries = 3` is elided.

```snap
tests/snap/code_completion/functions_snippets/06_default_argument.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variadic signature**

A trailing `...` shows in the parameter detail

```snap
tests/snap/code_completion/functions_snippets/07_variadic_signature.cpp
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

## Templates and concepts

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

## Filtering and ranking

<!-- BEGIN GENERATED ITEMS: filtering_ranking -->

<!-- BEGIN CAPABILITY: supported -->

**Underscore filtering**

Underscore-prefixed internal symbols hide unless the typed prefix itself starts with one

```snap
tests/snap/code_completion/filtering_ranking/01_underscore_filter.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated tagging**

A [[deprecated]] candidate carries the Deprecated tag, its plain sibling does not

```snap
tests/snap/code_completion/filtering_ranking/02_deprecated_tag.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Word-boundary fuzzy match**

Prefix `fb` matches the word starts of `foo_bar_baz`

`frobnicate` is only a weak scattered subsequence of `fb` and is dropped;
`foo_bar_baz` matches on the `foo`/`bar` word boundaries and survives.

```snap
tests/snap/code_completion/filtering_ranking/03_fuzzy_word_boundary.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Case-insensitive prefix**

A lowercase prefix matches a mixed-case identifier

```snap
tests/snap/code_completion/filtering_ranking/04_case_insensitive.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Prefix outranks subsequence**

An exact-prefix candidate sorts above a scattered subsequence match

For prefix `fo`, `format_output` is a true prefix and outscores
`fast_math_operation`, which only matches as a subsequence.

```snap
tests/snap/code_completion/filtering_ranking/05_prefix_beats_subsequence.cpp
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

## Auto-include insertion

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

## Documentation

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

## Trigger characters

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

## Protocol

- [ ] `completionItem/resolve` for lazy-loading documentation and details
- [ ] `CompletionList.isIncomplete` flag for incremental filtering
- [ ] `commitCharacters` for auto-accepting completions on specific keystrokes
- [ ] `filterText` / `sortText` for client-side re-filtering
