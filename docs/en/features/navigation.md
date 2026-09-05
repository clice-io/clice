# Code Navigation

## Go to Definition

<!-- BEGIN GENERATED ITEMS: go_to_definition -->

<!-- BEGIN CAPABILITY: supported -->

**Cross-TU go-to-definition**

A use in one translation unit resolves to the definition supplied by
a sibling source — the answer spans the project, not the current
file alone.

```snap-navigation
feature: navigation
code: |
  #include "shared.h"

  int run(int value) {
      return §(cross_use)transform(value);
  }
file lib.cpp: |
  #include "shared.h"

  int transform(int value) {
      return value * 2;
  }
file shared.h: |
  #pragma once

  int transform(int value);
snapshot: |
  cross_use:
    definition:
      - { file: "${WS}/go_to_definition/01_def_cross_tu/lib.cpp", range: "2:4-2:13" }
    declaration:
      - { file: "${WS}/go_to_definition/01_def_cross_tu/lib.cpp", range: "2:4-2:13" }
      - { file: "${WS}/go_to_definition/01_def_cross_tu/shared.h", range: "2:4-2:13" }
    references:
      - { file: "${WS}/go_to_definition/01_def_cross_tu/lib.cpp", range: "2:4-2:13" }
      - { file: "${WS}/go_to_definition/01_def_cross_tu/main.cpp", range: "12:11-12:20" }
      - { file: "${WS}/go_to_definition/01_def_cross_tu/shared.h", range: "2:4-2:13" }
    callHierarchy:
      - { name: "transform", kind: Function, file: "${WS}/go_to_definition/01_def_cross_tu/lib.cpp", range: "2:4-2:13" }
    incomingCalls:
      - { name: "run", kind: Function, file: "${WS}/go_to_definition/01_def_cross_tu/main.cpp", range: "11:4-11:7", fromRanges: ["12:11-12:27"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definition and declaration alternate at the cursor site**

On a use, go-to-definition reaches the definition. Invoked on the
definition it steps to the declaration, and on the declaration it
steps to the definition — the two sites alternate. A symbol defined
inline, with no separate declaration, keeps its definition as the
answer.

```snap-navigation
feature: navigation
code: |
  int §(decl)scale(int value);

  int §(def)scale(int value) {
      return value * 2;
  }

  int apply(int value) {
      return §(use)scale(value);
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    declaration:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    references:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "18:11-18:16" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    incomingCalls:
      - { name: "apply", kind: Function, file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "17:4-17:9", fromRanges: ["18:11-18:23"] }

  def:
    definition:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "18:11-18:16" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    incomingCalls:
      - { name: "apply", kind: Function, file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "17:4-17:9", fromRanges: ["18:11-18:23"] }

  use:
    definition:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    declaration:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    references:
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
      - { file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "18:11-18:16" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "13:4-13:9" }
    incomingCalls:
      - { name: "apply", kind: Function, file: "${WS}/go_to_definition/02_def_decl_alternate.cpp", range: "17:4-17:9", fromRanges: ["18:11-18:23"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration-only symbols navigate to their declaration**

Symbols that carry only a declaration — pure virtuals, `extern`
variables, in-class static constants — resolve to that declaration
instead of returning nothing.

```snap-navigation
feature: navigation
code: |
  extern int threshold;

  int probe(int value);

  struct Screen {
      static const int margin = 4;
      virtual void refresh() = 0;
  };

  int watch(Screen& screen, int value) {
      screen.§(pure_use)refresh();
      return §(fn_use)probe(value) + §(var_use)threshold + Screen::§(const_use)margin;
  }
snapshot: |
  const_use:
    definition:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "14:21-14:27" }
    declaration:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "14:21-14:27" }
    references:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "14:21-14:27" }
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "20:46-20:52" }

  fn_use:
    definition:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "20:11-20:16" }
    callHierarchy:
      - { name: "probe", kind: Function, file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "watch", kind: Function, file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "18:4-18:9", fromRanges: ["20:11-20:23"] }

  pure_use:
    definition:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "15:17-15:24" }
    declaration:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "15:17-15:24" }
    references:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "15:17-15:24" }
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "19:11-19:18" }
    callHierarchy:
      - { name: "refresh", kind: Method, file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "15:17-15:24" }
    incomingCalls:
      - { name: "watch", kind: Function, file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "18:4-18:9", fromRanges: ["19:4-19:20"] }

  var_use:
    definition:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "9:11-9:20" }
    declaration:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "9:11-9:20" }
    references:
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "9:11-9:20" }
      - { file: "${WS}/go_to_definition/03_def_declaration_only.cpp", range: "20:26-20:35" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Go-to-definition on `#include` directives**

Invoked on an `#include` line, go-to-definition opens the included
file. This works for the leading includes compiled into the preamble
(the PCH) as well as ordinary ones later in the file.

```snap-navigation
feature: navigation
code: |
  #include §(preamble_include)"panel.h"

  int build() {
      return dimension();
  }

  #include §(late_include)"extra.h"

  int total() {
      return build() + spacing();
  }
file extra.h: |
  inline int spacing() {
      return 2;
  }
file panel.h: |
  #pragma once

  int dimension();
snapshot: |
  late_include:
    definition:
      - { file: "${WS}/go_to_definition/04_def_include/extra.h", range: "0:0-0:0" }

  preamble_include:
    definition:
      - { file: "${WS}/go_to_definition/04_def_include/panel.h", range: "0:0-0:0" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Local variables and parameters navigate to their declaration**

Go-to-definition on a local variable or parameter jumps to its
declaration inside the function body.

```snap-navigation
feature: navigation
code: |
  int accumulate(int base) {
      int total = base;
      total = §(local_use)total + §(param_use)base;
      return total;
  }
snapshot: |
  local_use:
    definition:
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "9:8-9:13" }
    declaration:
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "9:8-9:13" }
    references:
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "9:8-9:13" }
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "10:4-10:9" }
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "10:12-10:17" }
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "11:11-11:16" }

  param_use:
    definition:
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "8:19-8:23" }
    declaration:
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "8:19-8:23" }
    references:
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "8:19-8:23" }
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "9:16-9:20" }
      - { file: "${WS}/go_to_definition/05_def_local_symbol.cpp", range: "10:20-10:24" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Navigate through macro wrappers to the underlying declaration**

A name spelled in a macro argument anchors at its spelling, so
definition and declaration alternate there exactly as at a plain
site, and a later use resolves through the wrapper to the function it
declares.

```snap-navigation
feature: navigation
code: |
  #define DECLARE_HOOK(name) int name(int value)

  DECLARE_HOOK(§(decl)notify);

  DECLARE_HOOK(§(def)notify) {
      return value + 1;
  }

  int trigger(int value) {
      return §(use)notify(value);
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    declaration:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    references:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "12:13-12:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "19:11-19:17" }
    callHierarchy:
      - { name: "notify", kind: Function, file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    incomingCalls:
      - { name: "trigger", kind: Function, file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "18:4-18:11", fromRanges: ["19:11-19:24"] }

  def:
    definition:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "12:13-12:19" }
    declaration:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "12:13-12:19" }
    references:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "12:13-12:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "19:11-19:17" }
    callHierarchy:
      - { name: "notify", kind: Function, file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    incomingCalls:
      - { name: "trigger", kind: Function, file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "18:4-18:11", fromRanges: ["19:11-19:24"] }

  use:
    definition:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    declaration:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "12:13-12:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    references:
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "12:13-12:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
      - { file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "19:11-19:17" }
    callHierarchy:
      - { name: "notify", kind: Function, file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "14:13-14:19" }
    incomingCalls:
      - { name: "trigger", kind: Function, file: "${WS}/go_to_definition/06_def_macro_wrapper.cpp", range: "18:4-18:11", fromRanges: ["19:11-19:24"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Names conjured by a macro body or token paste anchor at the invocation**

A name assembled by token paste has no spelling of its own in the
source, so it anchors at the macro invocation that creates it: the
invocation is its definition site, and a plain use of the name jumps
back to that invocation.

```snap-navigation
feature: navigation
code: |
  #define MAKE_FLAG(name) bool flag_##name = false

  §(flag_site)MAKE_FLAG(verbose);

  bool read_flag() {
      return §(flag_use)flag_verbose;
  }
snapshot: |
  flag_site:
    definition:
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "10:8-10:17" }
    declaration:
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "10:8-10:17" }
    references:
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "10:8-10:17" }
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "12:0-12:9" }

  flag_use:
    definition:
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "12:0-12:9" }
    declaration:
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "12:0-12:9" }
    references:
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "12:0-12:9" }
      - { file: "${WS}/go_to_definition/07_def_macro_generated.cpp", range: "15:11-15:23" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Tokens inside a `#define` body carry no navigation of their own**

A token written inside a macro body has no meaning until an expansion
assigns one, so navigation on it yields nothing, while the invocation
token always resolves to the macro being expanded.

```snap-navigation
feature: navigation
code: |
  #define DEFINE_COUNTER int §(body_token)counter = 0

  §(invocation)DEFINE_COUNTER;
snapshot: |
  body_token: none

  invocation:
    definition:
      - { file: "${WS}/go_to_definition/08_def_macro_body.cpp", range: "9:8-9:22" }
    declaration:
      - { file: "${WS}/go_to_definition/08_def_macro_body.cpp", range: "9:8-9:22" }
    references:
      - { file: "${WS}/go_to_definition/08_def_macro_body.cpp", range: "9:8-9:22" }
      - { file: "${WS}/go_to_definition/08_def_macro_body.cpp", range: "11:0-11:14" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Error recovery**

Navigate to a variable whose type is unresolved

When a variable's type name fails to resolve, go-to-definition on a
later use of the variable currently returns nothing, even though the
variable's own declaration is still recorded.

```snap-navigation
feature: navigation
code: |
  Unresolved handle;  // 'Unresolved' does not name a type

  void read() {
      (void) handle;  // go-to-def on handle → the declaration above
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent member navigation in uninstantiated templates**

Inside a template that is never instantiated, a member accessed on an
object of a dependent type resolves to the member declared on the
corresponding class template.

```snap-navigation
feature: navigation
code: |
  template <typename T>
  struct Sink {
      void §(member_decl)push(T value);
  };

  template <typename T>
  void drain(Sink<T>& sink, T value) {
      sink.§(member_call)push(value);
  }
snapshot: |
  member_call:
    definition:
      - { file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
    declaration:
      - { file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
    references:
      - { file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
    callHierarchy:
      - { name: "push", kind: Method, file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }

  member_decl:
    definition:
      - { file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
    declaration:
      - { file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
    references:
      - { file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
    callHierarchy:
      - { name: "push", kind: Method, file: "${WS}/go_to_definition/10_def_dependent_type.cpp", range: "11:9-11:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#212 -->

**Template specialization navigates to the primary template**

Go-to-definition on the name of an explicit specialization resolves to
the specialization itself; stepping from it to the primary template it
specializes is not offered.

```snap-navigation
feature: navigation
code: |
  template <typename T>
  struct Formatter {}; // primary template

  template <>
  struct Formatter<int> {}; // go-to-def on Formatter → primary template
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2055 -->

**`auto` keyword navigates to the deduced type**

Go-to-definition on the `auto` keyword should reach the type it was
deduced to; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Widget {};

  Widget make_widget();

  void use() {
      auto widget = make_widget(); // go-to-def on auto → Widget
  }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Implicit Code Navigation

Navigate to definitions of implicitly invoked code. In C++ many constructs generate hidden calls to constructors, operators, conversions, etc. Navigating from the syntactic construct (a brace, a keyword, an operator token) to the actual function being called is essential for understanding what code is really executing.

Implicit navigation requires an unambiguous source token — patterns where the token already has a well-defined go-to-def target (e.g., a variable name always goes to its declaration) cannot be repurposed for implicit call navigation.

<!-- BEGIN GENERATED ITEMS: implicit_code_navigation -->

<!-- BEGIN CAPABILITY: unsupported -->

**`override` / `final`**

Navigate to the overridden base method

Go-to-definition on the `override` or `final` specifier should reach the
base class virtual method it overrides; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Base {
      virtual void draw();
      virtual void paint();
  };

  struct Derived : Base {
      void draw() override;  // go-to-def on override → Base::draw
      void paint() final;    // go-to-def on final → Base::paint
  };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**`break` / `continue`**

Navigate to the enclosing loop or switch head

Go-to-definition on `break` or `continue` should reach the head of the
loop or switch it controls; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  void loop() {
      for (int i = 0; i < 10; i += 1) {
          if (i == 5) break;  // go-to-def on break → the for loop
          continue;           // go-to-def on continue → the for loop
      }
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructor calls**

From parentheses or braces to the selected constructor

Go-to-definition on the opening parenthesis or brace of a constructor
call reaches the constructor overload resolution selected, for both the
`T(args)` and `T{args}` forms.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      Widget(int w, int h);
  };

  void build() {
      Widget a§(paren)(800, 600);
      Widget b§(brace){800, 600};
  }
snapshot: |
  brace:
    definition:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "9:7-9:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "14:12-14:13" }
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "15:12-15:13" }
    callHierarchy:
      - { name: "Widget", kind: Method, file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
    incomingCalls:
      - { name: "build", kind: Function, file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "13:5-13:10", fromRanges: ["14:11-14:22", "15:11-15:22"] }

  paren:
    definition:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "9:7-9:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "14:12-14:13" }
      - { file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "15:12-15:13" }
    callHierarchy:
      - { name: "Widget", kind: Method, file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "10:4-10:10" }
    incomingCalls:
      - { name: "build", kind: Function, file: "${WS}/implicit_code_navigation/03_implicit_constructor_call.cpp", range: "13:5-13:10", fromRanges: ["14:11-14:22", "15:11-15:22"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Copy/move construction and assignment**

To the constructor or assignment operator

Go-to-definition on the `=` of an assignment reaches the assignment
operator. The `=` that introduces a copy- or move-initialization
(`T b = a;`) is initialization syntax rather than an operator call and is
not yet resolved.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      Widget(int v);
      Widget(const Widget& other);
      Widget(Widget&& other);
      Widget& operator=(const Widget& other);
  };

  void copies(Widget a) {
      Widget b §(copy_eq)= a;
      Widget c §(move_eq)= static_cast<Widget&&>(a);
      b §(assign_eq)= c;
  }
snapshot: |
  assign_eq:
    definition:
      - { file: "${WS}/implicit_code_navigation/04_implicit_copy_move.cpp", range: "14:12-14:20" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/04_implicit_copy_move.cpp", range: "14:12-14:20" }
    references:
      - { file: "${WS}/implicit_code_navigation/04_implicit_copy_move.cpp", range: "14:12-14:20" }
      - { file: "${WS}/implicit_code_navigation/04_implicit_copy_move.cpp", range: "20:6-20:7" }
    callHierarchy:
      - { name: "operator=", kind: Operator, file: "${WS}/implicit_code_navigation/04_implicit_copy_move.cpp", range: "14:12-14:20" }
    incomingCalls:
      - { name: "copies", kind: Function, file: "${WS}/implicit_code_navigation/04_implicit_copy_move.cpp", range: "17:5-17:11", fromRanges: ["20:4-20:9"] }

  copy_eq: none

  move_eq: none
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**CTAD**

Navigate to the selected constructor

When class template argument deduction picks a specialization, go-to-
definition on the constructor call reaches the constructor that was
selected, not merely the class template.

```snap-navigation
feature: navigation
code: |
  template <typename T>
  struct Box {
      Box(T input) : value(input) {}
      T value;
  };

  template <typename T>
  Box(T) -> Box<T>;

  void use() {
      Box b§(ctad_paren)(7);
  }
snapshot: |
  ctad_paren:
    definition:
      - { file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "11:4-11:7" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "11:4-11:7" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "10:7-10:10" }
    references:
      - { file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "11:4-11:7" }
      - { file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "19:9-19:10" }
    callHierarchy:
      - { name: "Box", kind: Method, file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "11:4-11:7" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/implicit_code_navigation/05_implicit_ctad.cpp", range: "18:5-18:8", fromRanges: ["19:8-19:12"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Aggregate initialization**

Navigate to the struct definition

An aggregate has no constructor, so go-to-definition on its initializer
brace reaches the aggregate's definition.

```snap-navigation
feature: navigation
code: |
  struct Point {
      int x;
      int y;
  };

  void use() {
      auto p = Point§(agg_brace){1, 2};
  }
snapshot: |
  agg_brace:
    definition:
      - { file: "${WS}/implicit_code_navigation/06_implicit_aggregate_init.cpp", range: "8:7-8:12" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/06_implicit_aggregate_init.cpp", range: "8:7-8:12" }
    references:
      - { file: "${WS}/implicit_code_navigation/06_implicit_aggregate_init.cpp", range: "8:7-8:12" }
      - { file: "${WS}/implicit_code_navigation/06_implicit_aggregate_init.cpp", range: "14:13-14:18" }
    typeHierarchy:
      - { name: "Point", kind: Struct, file: "${WS}/implicit_code_navigation/06_implicit_aggregate_init.cpp", range: "8:7-8:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`delete` expression**

Navigate to the destructor

Go-to-definition on `delete` should reach the destructor it runs; today
it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      ~Widget();
  };

  void dispose(Widget* widget) {
      delete widget;  // go-to-def on delete → Widget::~Widget
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`new` expression**

Navigate to the constructor and overloaded `operator new`

Go-to-definition on `new` reaches the class's overloaded `operator new`.
The constructor invoked by the same expression is not part of the reply.

```snap-navigation
feature: navigation
code: |
  struct Pool {
      Pool();
      static void* operator new(decltype(sizeof(0)) size);
  };

  void make() {
      Pool* p = §(new_kw)new Pool();
  }
snapshot: |
  new_kw:
    definition:
      - { file: "${WS}/implicit_code_navigation/08_implicit_new_ctor.cpp", range: "10:17-10:25" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/08_implicit_new_ctor.cpp", range: "10:17-10:25" }
    references:
      - { file: "${WS}/implicit_code_navigation/08_implicit_new_ctor.cpp", range: "10:17-10:25" }
      - { file: "${WS}/implicit_code_navigation/08_implicit_new_ctor.cpp", range: "14:14-14:17" }
    callHierarchy:
      - { name: "operator new", kind: Operator, file: "${WS}/implicit_code_navigation/08_implicit_new_ctor.cpp", range: "10:17-10:25" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Member initializer list**

Navigate to base and member constructors

The base and member constructors run by an initializer list are reached
from the opening parenthesis of each initializer. The initializer name
itself resolves to the base type or the member, so navigation to the
constructor goes through the parenthesis.

```snap-navigation
feature: navigation
code: |
  struct Base {
      Base(int x);
  };

  struct Logger {
      Logger(int level);
  };

  struct App : Base {
      Logger logger;
      App() : §(base_init)Base§(base_paren)(42), §(member_init)logger§(member_paren)(1) {}
  };
snapshot: |
  base_init:
    definition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "10:7-10:11" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "10:7-10:11" }
    implementation:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "18:7-18:10" }
    references:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "10:7-10:11" }
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "18:13-18:17" }
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "20:12-20:16" }
    typeHierarchy:
      - { name: "Base", kind: Struct, file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "10:7-10:11" }
    subtypes:
      - { name: "App", kind: Struct, file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "18:7-18:10" }

  base_paren:
    definition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "11:4-11:8" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "11:4-11:8" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "10:7-10:11" }
    references:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "11:4-11:8" }
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "20:16-20:17" }
    callHierarchy:
      - { name: "Base", kind: Method, file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "11:4-11:8" }
    incomingCalls:
      - { name: "App", kind: Method, file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "20:4-20:7", fromRanges: ["20:12-20:20"] }

  member_init:
    definition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "19:11-19:17" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "19:11-19:17" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "14:7-14:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "19:11-19:17" }
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "20:22-20:28" }

  member_paren:
    definition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "15:4-15:10" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "15:4-15:10" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "14:7-14:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "15:4-15:10" }
      - { file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "20:28-20:29" }
    callHierarchy:
      - { name: "Logger", kind: Method, file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "15:4-15:10" }
    incomingCalls:
      - { name: "App", kind: Method, file: "${WS}/implicit_code_navigation/09_implicit_member_init.cpp", range: "20:4-20:7", fromRanges: ["20:22-20:31"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Delegating constructors**

Navigate to the target constructor

A delegating constructor's target is reached from the opening parenthesis
of the delegated call. The constructor name itself resolves to the class
type, so navigation to the target constructor goes through the
parenthesis.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      Widget(int w, int h);
      Widget() : §(delegate)Widget§(delegate_paren)(0, 0) {}
  };
snapshot: |
  delegate:
    definition:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "10:7-10:13" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "10:7-10:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "10:7-10:13" }
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "12:15-12:21" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "10:7-10:13" }

  delegate_paren:
    definition:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "11:4-11:10" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "11:4-11:10" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "10:7-10:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "11:4-11:10" }
      - { file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "12:21-12:22" }
    callHierarchy:
      - { name: "Widget", kind: Method, file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "11:4-11:10" }
    incomingCalls:
      - { name: "Widget", kind: Method, file: "${WS}/implicit_code_navigation/10_implicit_delegating_ctor.cpp", range: "12:4-12:10", fromRanges: ["12:15-12:27"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Inherited constructors**

Navigate to the base constructors brought in by `using`

Go-to-definition on an inherited-constructor declaration
(`using Base::Base;`) reaches a base constructor. When the base declares
several constructors the reply resolves to one of them rather than
listing the whole set.

```snap-navigation
feature: navigation
code: |
  struct Base {
      Base(int x);
      Base(int x, int y);
  };

  struct Derived : Base {
      using Base::§(inherit)Base;
  };
snapshot: |
  inherit:
    definition:
      - { file: "${WS}/implicit_code_navigation/11_implicit_inherited_ctor.cpp", range: "11:4-11:8" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/11_implicit_inherited_ctor.cpp", range: "11:4-11:8" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/11_implicit_inherited_ctor.cpp", range: "10:7-10:11" }
    references:
      - { file: "${WS}/implicit_code_navigation/11_implicit_inherited_ctor.cpp", range: "11:4-11:8" }
    callHierarchy:
      - { name: "Base", kind: Method, file: "${WS}/implicit_code_navigation/11_implicit_inherited_ctor.cpp", range: "11:4-11:8" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Return value implicit construction**

Navigate to the constructor

A braced `return {args}` implicitly constructs the function's return
type; go-to-definition on the brace reaches the selected constructor.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      Widget(int w, int h);
  };

  Widget create() {
      return §(ret_brace){800, 600};
  }
snapshot: |
  ret_brace:
    definition:
      - { file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "9:4-9:10" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "9:4-9:10" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "9:4-9:10" }
      - { file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "13:11-13:12" }
    callHierarchy:
      - { name: "Widget", kind: Method, file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "9:4-9:10" }
    incomingCalls:
      - { name: "create", kind: Function, file: "${WS}/implicit_code_navigation/12_implicit_return_construction.cpp", range: "12:7-12:13", fromRanges: ["13:11-13:21"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Lambda init-capture**

Navigate to the constructor

Go-to-definition on the `=` of a lambda init-capture should reach the
constructor that builds the captured value; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      Widget(int v);
      Widget(Widget&& other);
  };

  void use(Widget w) {
      // go-to-def on = → Widget(Widget&&)
      auto f = [x = static_cast<Widget&&>(w)] {};
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overloaded operators**

From the operator token to its definition

Go-to-definition on an overloaded operator token reaches the operator's
definition. The binary, subscript, call and arrow operators (`+`, `[]`,
`()`, `->`) are all resolved.

```snap-navigation
feature: navigation
code: |
  struct Iterator {
      int value;
  };

  struct Vec {
      Vec operator+(const Vec& other) const;
      int operator[](int index) const;
      int operator()(int a, int b) const;
      Iterator* operator->();
  };

  void use(Vec a, Vec b) {
      Vec c = a §(plus)+ b;
      int e = a§(subscript)[0];
      int f = a§(call)(1, 2);
      a§(arrow)->value;
  }
snapshot: |
  arrow:
    definition:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "17:14-17:22" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "17:14-17:22" }
    references:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "17:14-17:22" }
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "24:5-24:7" }
    callHierarchy:
      - { name: "operator->", kind: Operator, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "17:14-17:22" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "20:5-20:8", fromRanges: ["24:4-24:7"] }

  call:
    definition:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "16:8-16:16" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "16:8-16:16" }
    references:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "16:8-16:16" }
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "23:13-23:14" }
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "23:18-23:19" }
    callHierarchy:
      - { name: "operator()", kind: Operator, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "16:8-16:16" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "20:5-20:8", fromRanges: ["23:12-23:19"] }

  plus:
    definition:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "14:8-14:16" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "14:8-14:16" }
    references:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "14:8-14:16" }
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "21:14-21:15" }
    callHierarchy:
      - { name: "operator+", kind: Operator, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "14:8-14:16" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "20:5-20:8", fromRanges: ["21:12-21:17"] }

  subscript:
    definition:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "15:8-15:16" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "15:8-15:16" }
    references:
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "15:8-15:16" }
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "22:13-22:14" }
      - { file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "22:15-22:16" }
    callHierarchy:
      - { name: "operator[]", kind: Operator, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "15:8-15:16" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/implicit_code_navigation/14_implicit_operator_call.cpp", range: "20:5-20:8", fromRanges: ["22:12-22:16"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C++20 rewritten operators**

Navigate to the operator the rewrite uses

For a comparison synthesized by the C++20 rewrite rules, go-to-definition
on the written operator reaches the operator that actually implements it:
`!=` reaches `operator==`, and `>` reaches `operator<=>`.

```snap-navigation
feature: navigation
code: |
  namespace std {
  struct strong_ordering {
      int n;
      constexpr operator int() const { return n; }
      static const strong_ordering equal, greater, less;
  };
  constexpr strong_ordering strong_ordering::equal = {0};
  constexpr strong_ordering strong_ordering::greater = {1};
  constexpr strong_ordering strong_ordering::less = {-1};
  }

  struct S {
      int value;
      bool operator==(const S& other) const;
      auto operator<=>(const S& other) const = default;
  };

  void use(S a, S b) {
      bool ne = a §(neq)!= b;
      bool gt = a §(gt)> b;
  }
snapshot: |
  gt:
    definition:
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "23:9-23:17" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "23:9-23:17" }
    references:
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "23:9-23:17" }
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "28:16-28:17" }
    callHierarchy:
      - { name: "operator<=>", kind: Operator, file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "23:9-23:17" }

  neq:
    definition:
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "22:9-22:17" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "22:9-22:17" }
    references:
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "22:9-22:17" }
      - { file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "27:16-27:18" }
    callHierarchy:
      - { name: "operator==", kind: Operator, file: "${WS}/implicit_code_navigation/15_implicit_rewritten_operator.cpp", range: "22:9-22:17" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**User-defined literals**

Navigate to the literal operator

Go-to-definition on a user-defined-literal suffix should reach its
`operator""`; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Duration {
      unsigned long long ticks;
  };

  Duration operator""_ms(unsigned long long value);

  void use() {
      Duration d = 500_ms;  // go-to-def on _ms → operator""_ms
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1931 -->

**Implicit conversion operators**

From a conversion context to the operator

Go-to-definition from a context that runs a user-defined conversion (a
condition, `!`, an explicit `bool(...)`) should reach the conversion
operator; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Guard {
      explicit operator bool() const;
  };

  void use(Guard g) {
      if (g) {}      // go-to-def on ( → Guard::operator bool
      bool ok = !g;  // go-to-def on ! → Guard::operator bool
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Casts invoking a constructor or conversion operator**

A `static_cast` that constructs its target reaches the selected
constructor. A `static_cast` that runs a user-defined conversion operator
does not yet reach the operator.

```snap-navigation
feature: navigation
code: |
  struct Meters {
      explicit operator double() const;
  };

  struct Foo {
      explicit Foo(int value);
  };

  void use(Meters m) {
      double d = §(cast_conv)static_cast<double>(m);
      Foo f = §(cast_ctor)static_cast<Foo>(42);
  }
snapshot: |
  cast_conv: none

  cast_ctor:
    definition:
      - { file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "14:13-14:16" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "14:13-14:16" }
    typeDefinition:
      - { file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "13:7-13:10" }
    references:
      - { file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "14:13-14:16" }
      - { file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "19:12-19:23" }
    callHierarchy:
      - { name: "Foo", kind: Method, file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "14:13-14:16" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/implicit_code_navigation/18_implicit_cast_conversion.cpp", range: "17:5-17:8", fromRanges: ["19:12-19:32"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Range-based for**

Navigate to `begin()` / `end()`

Go-to-definition on the `:` of a range-based for should reach the
`begin()` / `end()` chosen for the range; today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Iterator {
      int operator*() const;
      Iterator& operator++();
      bool operator!=(const Iterator& other) const;
  };

  struct Range {
      Iterator begin();
      Iterator end();
  };

  void use(Range r) {
      for (int x : r) {}  // go-to-def on : → Range::begin / Range::end
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Structured bindings**

Navigate to the underlying accessors or fields

Go-to-definition on a structured binding name resolves to the binding
itself rather than the underlying field or accessor it names.

```snap-navigation
feature: navigation
code: |
  struct Pair {
      int first;
      int second;
  };

  void use(Pair p) {
      // go-to-def on a → Pair::first, on b → Pair::second
      auto [a, b] = p;
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`co_await` / `co_yield` / `co_return`**

Navigate to the awaiter or promise method

Go-to-definition on `co_yield` reaches the promise's `yield_value`. The
`co_await` and `co_return` keywords do not yet reach the awaiter's or
promise's methods.

```snap-navigation
feature: navigation
code: |
  namespace std {
  template <typename Ret, typename...>
  struct coroutine_traits {
      using promise_type = typename Ret::promise_type;
  };
  template <typename = void>
  struct coroutine_handle {
      coroutine_handle() = default;
      template <typename Promise>
      coroutine_handle(coroutine_handle<Promise>) noexcept;
      static coroutine_handle from_address(void*) noexcept;
  };
  struct suspend_never {
      bool await_ready() const noexcept;
      void await_suspend(coroutine_handle<>) const noexcept;
      void await_resume() const noexcept;
  };
  }

  struct Awaiter {
      bool await_ready() const noexcept;
      void await_suspend(std::coroutine_handle<>) const noexcept;
      int await_resume() const noexcept;
  };

  struct Task {
      struct promise_type {
          Task get_return_object();
          std::suspend_never initial_suspend();
          std::suspend_never final_suspend() noexcept;
          Awaiter yield_value(int value);
          void return_value(int value);
          void unhandled_exception();
      };
  };

  Task example() {
      §(co_await_kw)co_await Awaiter{};
      §(co_yield_kw)co_yield 1;
      §(co_return_kw)co_return 2;
  }
snapshot: |
  co_await_kw: none

  co_return_kw: none

  co_yield_kw:
    definition:
      - { file: "${WS}/implicit_code_navigation/21_implicit_coroutine.cpp", range: "39:16-39:27" }
    declaration:
      - { file: "${WS}/implicit_code_navigation/21_implicit_coroutine.cpp", range: "39:16-39:27" }
    references:
      - { file: "${WS}/implicit_code_navigation/21_implicit_coroutine.cpp", range: "39:16-39:27" }
      - { file: "${WS}/implicit_code_navigation/21_implicit_coroutine.cpp", range: "47:4-47:12" }
    callHierarchy:
      - { name: "yield_value", kind: Method, file: "${WS}/implicit_code_navigation/21_implicit_coroutine.cpp", range: "39:16-39:27" }
    incomingCalls:
      - { name: "example", kind: Function, file: "${WS}/implicit_code_navigation/21_implicit_coroutine.cpp", range: "45:5-45:12", fromRanges: ["47:4-47:14"] }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Declaration

Navigate from a symbol usage or definition to its declaration. In C++, many entities have separate declarations and definitions.

clice returns the declaration locations plus the definition — symbols defined inline have no separate declaration — minus the site the cursor already stands on, so declaration and definition sites alternate just like go-to-definition.

<!-- BEGIN GENERATED ITEMS: go_to_declaration -->

<!-- BEGIN CAPABILITY: supported -->

**Cross-TU go-to-declaration**

Go-to-declaration on a use resolves sites in other files: the
prototype lives in a shared header and the out-of-line definition in a
sibling source, and both are offered from a use in another file.

```snap-navigation
feature: navigation
code: |
  #include "shared.h"

  int run(int value) {
      return §(use)scale(value);
  }
file lib.cpp: |
  #include "shared.h"

  int scale(int value) {
      return value * 2;
  }
file shared.h: |
  #pragma once

  int scale(int value);
snapshot: |
  use:
    definition:
      - { file: "${WS}/go_to_declaration/01_decl_cross_tu/lib.cpp", range: "2:4-2:9" }
    declaration:
      - { file: "${WS}/go_to_declaration/01_decl_cross_tu/lib.cpp", range: "2:4-2:9" }
      - { file: "${WS}/go_to_declaration/01_decl_cross_tu/shared.h", range: "2:4-2:9" }
    references:
      - { file: "${WS}/go_to_declaration/01_decl_cross_tu/lib.cpp", range: "2:4-2:9" }
      - { file: "${WS}/go_to_declaration/01_decl_cross_tu/main.cpp", range: "12:11-12:16" }
      - { file: "${WS}/go_to_declaration/01_decl_cross_tu/shared.h", range: "2:4-2:9" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/go_to_declaration/01_decl_cross_tu/lib.cpp", range: "2:4-2:9" }
    incomingCalls:
      - { name: "run", kind: Function, file: "${WS}/go_to_declaration/01_decl_cross_tu/main.cpp", range: "11:4-11:7", fromRanges: ["12:11-12:23"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functions**

From a use or out-of-line definition to the prototype

Go-to-declaration reaches a function's prototype both from a call site
and from the out-of-line definition — the two non-cursor sites the
prototype alternates with.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      void §(decl)draw();
  };

  void Widget::§(def)draw() {}

  void render(Widget& widget) {
      widget.§(use)draw();
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    declaration:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    references:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "10:9-10:13" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "16:11-16:15" }
    callHierarchy:
      - { name: "draw", kind: Method, file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    incomingCalls:
      - { name: "render", kind: Function, file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "15:5-15:11", fromRanges: ["16:4-16:17"] }

  def:
    definition:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "10:9-10:13" }
    declaration:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "10:9-10:13" }
    references:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "10:9-10:13" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "16:11-16:15" }
    callHierarchy:
      - { name: "draw", kind: Method, file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    incomingCalls:
      - { name: "render", kind: Function, file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "15:5-15:11", fromRanges: ["16:4-16:17"] }

  use:
    definition:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    declaration:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "10:9-10:13" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    references:
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "10:9-10:13" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
      - { file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "16:11-16:15" }
    callHierarchy:
      - { name: "draw", kind: Method, file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "13:13-13:17" }
    incomingCalls:
      - { name: "render", kind: Function, file: "${WS}/go_to_declaration/02_decl_function_prototype.cpp", range: "15:5-15:11", fromRanges: ["16:4-16:17"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Forward declarations of classes and structs**

A class with a forward declaration and a later definition offers both
from a use — the forward declaration stays part of the declaration set
rather than being dropped in favour of the definition.

```snap-navigation
feature: navigation
code: |
  struct §(fwd)Widget;

  struct §(def)Widget {
      int value;
  };

  class Panel;

  class Panel {
      int width;
  };

  int probe(§(use)Widget& widget, §(class_use)Panel& panel) {
      return widget.value;
  }
snapshot: |
  class_use:
    definition:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "17:6-17:11" }
    declaration:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "15:6-15:11" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "17:6-17:11" }
    references:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "15:6-15:11" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "17:6-17:11" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "21:26-21:31" }
    typeHierarchy:
      - { name: "Panel", kind: Class, file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "17:6-17:11" }

  def:
    definition:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "9:7-9:13" }
    declaration:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "9:7-9:13" }
    references:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "9:7-9:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "21:10-21:16" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }

  fwd:
    definition:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
    declaration:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
    references:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "9:7-9:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "21:10-21:16" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }

  use:
    definition:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
    declaration:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "9:7-9:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
    references:
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "9:7-9:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
      - { file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "21:10-21:16" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/go_to_declaration/03_decl_forward_class.cpp", range: "11:7-11:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static data member**

To the in-class declaration

A static data member is declared inside the class and defined out of
line; go-to-declaration on a use offers the in-class declaration
alongside the definition.

```snap-navigation
feature: navigation
code: |
  struct Config {
      static int §(decl)timeout;
  };

  int Config::§(def)timeout = 30;

  int read_config() {
      return Config::§(use)timeout;
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
    declaration:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
    references:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "10:15-10:22" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "16:19-16:26" }

  def:
    definition:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "10:15-10:22" }
    declaration:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "10:15-10:22" }
    references:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "10:15-10:22" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "16:19-16:26" }

  use:
    definition:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
    declaration:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "10:15-10:22" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
    references:
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "10:15-10:22" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "13:12-13:19" }
      - { file: "${WS}/go_to_declaration/04_decl_static_member.cpp", range: "16:19-16:26" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`extern` variable**

To the declaration

A use of an `extern` variable offers the `extern` declaration and
the defining declaration together, so the header-side declaration is
always reachable from a use.

```snap-navigation
feature: navigation
code: |
  extern int §(decl)log_level;

  int §(def)log_level = 0;

  int read_level() {
      return §(use)log_level;
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
    declaration:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
    references:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "9:11-9:20" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "14:11-14:20" }

  def:
    definition:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "9:11-9:20" }
    declaration:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "9:11-9:20" }
    references:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "9:11-9:20" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "14:11-14:20" }

  use:
    definition:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
    declaration:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "9:11-9:20" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
    references:
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "9:11-9:20" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "11:4-11:13" }
      - { file: "${WS}/go_to_declaration/05_decl_extern_variable.cpp", range: "14:11-14:20" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Multiple declarations**

Every declaration site

When an entity is declared in several places, go-to-declaration on a
use lists every declaration site, not only the nearest one.

```snap-navigation
feature: navigation
code: |
  int §(first)clamp(int value);
  int §(second)clamp(int value);

  int clamp(int value) {
      return value < 0 ? 0 : value;
  }

  int hold(int value) {
      return §(use)clamp(value);
  }
snapshot: |
  first:
    definition:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "9:4-9:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "8:4-8:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "9:4-9:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "16:11-16:16" }
    callHierarchy:
      - { name: "clamp", kind: Function, file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "hold", kind: Function, file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "15:4-15:8", fromRanges: ["16:11-16:23"] }

  second:
    definition:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "8:4-8:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "8:4-8:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "9:4-9:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "16:11-16:16" }
    callHierarchy:
      - { name: "clamp", kind: Function, file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "hold", kind: Function, file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "15:4-15:8", fromRanges: ["16:11-16:23"] }

  use:
    definition:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "8:4-8:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "9:4-9:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "8:4-8:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "9:4-9:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
      - { file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "16:11-16:16" }
    callHierarchy:
      - { name: "clamp", kind: Function, file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "hold", kind: Function, file: "${WS}/go_to_declaration/06_decl_multiple.cpp", range: "15:4-15:8", fromRanges: ["16:11-16:23"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration and definition with cosmetically different signatures**

Parameter names, and a top-level `const` on a parameter, are not part
of a function's type: the declaration and the definition below spell the
same function differently, yet go-to-declaration still connects a use to
the prototype.

```snap-navigation
feature: navigation
code: |
  int §(decl)render(int width, const int height);

  int §(def)render(int w, int h) {
      return w * h;
  }

  int use_render() {
      return §(use)render(800, 600);
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    declaration:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    references:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "10:4-10:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "17:11-17:17" }
    callHierarchy:
      - { name: "render", kind: Function, file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    incomingCalls:
      - { name: "use_render", kind: Function, file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "16:4-16:14", fromRanges: ["17:11-17:27"] }

  def:
    definition:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "10:4-10:10" }
    declaration:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "10:4-10:10" }
    references:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "10:4-10:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "17:11-17:17" }
    callHierarchy:
      - { name: "render", kind: Function, file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    incomingCalls:
      - { name: "use_render", kind: Function, file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "16:4-16:14", fromRanges: ["17:11-17:27"] }

  use:
    definition:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    declaration:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "10:4-10:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    references:
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "10:4-10:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
      - { file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "17:11-17:17" }
    callHierarchy:
      - { name: "render", kind: Function, file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "12:4-12:10" }
    incomingCalls:
      - { name: "use_render", kind: Function, file: "${WS}/go_to_declaration/07_decl_signature_mismatch.cpp", range: "16:4-16:14", fromRanges: ["17:11-17:27"] }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Implementation

<!-- BEGIN GENERATED ITEMS: go_to_implementation -->

<!-- BEGIN CAPABILITY: supported -->

**Override chain**

Each level of a chain to its own overriders

Along a three-level override chain, go-to-implementation from each method
reaches the override one level down — base to middle, middle to leaf.

```snap-navigation
feature: navigation
code: |
  struct Base {
      virtual void §(base)run() = 0;
  };

  struct Middle : Base {
      void §(middle)run() override {}
  };

  struct Leaf : Middle {
      void run() override {}
  };
snapshot: |
  base:
    definition:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "9:17-9:20" }
    declaration:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "9:17-9:20" }
    implementation:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "13:9-13:12" }
    references:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "9:17-9:20" }
    callHierarchy:
      - { name: "run", kind: Method, file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "9:17-9:20" }

  middle:
    definition:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "13:9-13:12" }
    declaration:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "13:9-13:12" }
    implementation:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "17:9-17:12" }
    references:
      - { file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "13:9-13:12" }
    callHierarchy:
      - { name: "run", kind: Method, file: "${WS}/go_to_implementation/01_impl_virtual_chain.cpp", range: "13:9-13:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Sibling overrides**

Every sibling override

Go-to-implementation on a virtual method lists every override across
the sibling derived classes.

```snap-navigation
feature: navigation
code: |
  struct Shape {
      virtual int §(base)area() = 0;
  };

  struct Circle : Shape {
      int area() override { return 1; }
  };

  struct Square : Shape {
      int area() override { return 2; }
  };

  struct Triangle : Shape {
      int area() override { return 3; }
  };
snapshot: |
  base:
    definition:
      - { file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "9:16-9:20" }
    declaration:
      - { file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "9:16-9:20" }
    implementation:
      - { file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "13:8-13:12" }
      - { file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "17:8-17:12" }
      - { file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "21:8-21:12" }
    references:
      - { file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "9:16-9:20" }
    callHierarchy:
      - { name: "area", kind: Method, file: "${WS}/go_to_implementation/02_impl_virtual_siblings.cpp", range: "9:16-9:20" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#854 -->

**Non-virtual function**

Declaration to out-of-line definition

Go-to-implementation on a non-virtual function declaration should reach
its out-of-line definition, behaving as a superset of go-to-definition;
today it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Widget {
      void draw();  // go-to-impl on draw → out-of-line definition below
  };

  void Widget::draw() {}
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Base class**

Every derived class

Go-to-implementation on a base class name lists the classes that derive
from it.

```snap-navigation
feature: navigation
code: |
  struct §(base)Base {};

  struct Circle : Base {};

  struct Square : Base {};
snapshot: |
  base:
    definition:
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "8:7-8:11" }
    declaration:
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "8:7-8:11" }
    implementation:
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "10:7-10:13" }
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "12:7-12:13" }
    references:
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "8:7-8:11" }
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "10:16-10:20" }
      - { file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "12:16-12:20" }
    typeHierarchy:
      - { name: "Base", kind: Struct, file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "8:7-8:11" }
    subtypes:
      - { name: "Circle", kind: Struct, file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "10:7-10:13" }
      - { name: "Square", kind: Struct, file: "${WS}/go_to_implementation/04_impl_base_derived.cpp", range: "12:7-12:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Template duck-type navigation**

From a dependent member call, go-to-implementation should list the
concrete methods of every known instantiation; the same applies to a
generic lambda's dependent calls. Today it returns nothing.

```snap-navigation
feature: navigation
code: |
  template <typename T>
  void process(T& obj) {
      obj.foo();  // go-to-impl on foo → A::foo (from the process(a) instantiation)
  }

  struct A {
      void foo() {}
  };

  void run(A a) {
      process(a);
  }

  void generic() {
      auto call = [](auto& x) { x.bar(); };  // go-to-impl on bar → the concrete bar
  }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Type Definition

Navigate to the type definition of a symbol. Applicable to variables, parameters, fields, and any other named entity that has a type. When the type is a type alias or a pointer-like wrapper, navigation should unwrap to the underlying/pointee type.

<!-- BEGIN GENERATED ITEMS: go_to_type_definition -->

<!-- BEGIN CAPABILITY: supported -->

**Variables and parameters**

Go-to-type-definition on a local variable or a parameter reaches the
definition of its type.

```snap-navigation
feature: navigation
code: |
  struct §(type)Widget {};

  Widget make_widget();

  int probe(Widget §(param)param) {
      Widget §(local)local = make_widget();
      return 0;
  }
snapshot: |
  local:
    definition:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "13:11-13:16" }
    declaration:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "13:11-13:16" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "13:11-13:16" }

  param:
    definition:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "12:17-12:22" }
    declaration:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "12:17-12:22" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "12:17-12:22" }

  type:
    definition:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "8:7-8:13" }
    declaration:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "8:7-8:13" }
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "10:0-10:6" }
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "12:10-12:16" }
      - { file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "13:4-13:10" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/go_to_type_definition/01_typedef_variables.cpp", range: "8:7-8:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class and struct fields**

Go-to-type-definition on a field access reaches the definition of the
field's type.

```snap-navigation
feature: navigation
code: |
  struct §(type)Logger {};

  class §(class_type)Store {};

  struct App {
      Logger logger;
      Store store;
  };

  int use(App& app) {
      app.§(field)logger;
      app.§(class_field)store;
      return 0;
  }
snapshot: |
  class_field:
    definition:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "14:10-14:15" }
    declaration:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "14:10-14:15" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "10:6-10:11" }
    references:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "14:10-14:15" }
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "19:8-19:13" }

  class_type:
    definition:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "10:6-10:11" }
    declaration:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "10:6-10:11" }
    references:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "10:6-10:11" }
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "14:4-14:9" }
    typeHierarchy:
      - { name: "Store", kind: Class, file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "10:6-10:11" }

  field:
    definition:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "13:11-13:17" }
    declaration:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "13:11-13:17" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "13:11-13:17" }
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "18:8-18:14" }

  type:
    definition:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "8:7-8:13" }
    declaration:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "8:7-8:13" }
      - { file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "13:4-13:10" }
    typeHierarchy:
      - { name: "Logger", kind: Struct, file: "${WS}/go_to_type_definition/02_typedef_field.cpp", range: "8:7-8:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`auto`-deduced variables**

Go-to-type-definition on an `auto`-deduced variable should reach the
deduced type's definition; today the variable carries no type relation,
so it returns nothing.

```snap-navigation
feature: navigation
code: |
  struct Widget {};

  Widget make_widget();

  void probe() {
      auto widget = make_widget();  // go-to-type-def on widget → Widget
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1026 -->

**Smart pointer to the pointee type**

Go-to-type-definition on a smart-pointer variable reaches the wrapper
type itself; unwrapping to the pointee type is not offered.

```snap-navigation
feature: navigation
code: |
  template <typename T>
  struct Ptr {
      T* operator->();
      T& operator*();
      T* raw;
  };

  struct §(type)Widget {};

  int use(Ptr<Widget> §(ptr)ptr) {
      return 0;
  }
snapshot: |
  ptr:
    definition:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "18:20-18:23" }
    declaration:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "18:20-18:23" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "10:7-10:10" }
    references:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "18:20-18:23" }

  type:
    definition:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "16:7-16:13" }
    declaration:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "16:7-16:13" }
    references:
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "16:7-16:13" }
      - { file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "18:12-18:18" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/go_to_type_definition/04_typedef_smart_pointer.cpp", range: "16:7-16:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Type aliases**

Go-to-type-definition on a variable of an aliased type reaches the
`using` or `typedef` declaration; it does not yet unwrap the alias to
the underlying type's definition.

```snap-navigation
feature: navigation
code: |
  struct §(underlying)Impl {};

  using §(alias)Handle = Impl;

  typedef Impl LegacyHandle;

  int use(Handle §(var)handle, LegacyHandle §(legacy)legacy) {
      return 0;
  }
snapshot: |
  alias:
    definition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "11:6-11:12" }
    declaration:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "11:6-11:12" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "9:7-9:11" }
    references:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "11:6-11:12" }
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:8-15:14" }

  legacy:
    definition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:36-15:42" }
    declaration:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:36-15:42" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "13:13-13:25" }
    references:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:36-15:42" }

  underlying:
    definition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "9:7-9:11" }
    declaration:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "9:7-9:11" }
    references:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "9:7-9:11" }
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "11:15-11:19" }
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "13:8-13:12" }
    typeHierarchy:
      - { name: "Impl", kind: Struct, file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "9:7-9:11" }

  var:
    definition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:15-15:21" }
    declaration:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:15-15:21" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "11:6-11:12" }
    references:
      - { file: "${WS}/go_to_type_definition/05_typedef_alias.cpp", range: "15:15-15:21" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured binding variables**

Go-to-type-definition on a structured binding reaches the definition of
the bound member's type.

```snap-navigation
feature: navigation
code: |
  struct §(type)Widget {};

  struct Pair {
      Widget first;
      int second;
  };

  Pair make_pair();

  int use() {
      auto [§(bound)widget, count] = make_pair();
      return 0;
  }
snapshot: |
  bound:
    definition:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "18:10-18:16" }
    declaration:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "18:10-18:16" }
    typeDefinition:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "18:10-18:16" }

  type:
    definition:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "8:7-8:13" }
    declaration:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "8:7-8:13" }
    references:
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "8:7-8:13" }
      - { file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "11:4-11:10" }
    typeHierarchy:
      - { name: "Widget", kind: Struct, file: "${WS}/go_to_type_definition/06_typedef_structured_binding.cpp", range: "8:7-8:13" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Find References

<!-- BEGIN GENERATED ITEMS: find_references -->

<!-- BEGIN CAPABILITY: supported -->

**Cross-TU find references**

Find references gathers uses from other files too: a function
defined in one source and called from a sibling reports both call
sites together with the declaration in the shared header, not only the
uses in the current file.

```snap-navigation
feature: navigation
code: |
  #include "shared.h"

  int run(int value) {
      return §(use)compute(value);
  }
file lib.cpp: |
  #include "shared.h"

  int compute(int value) {
      return value * 2;
  }

  int again(int value) {
      return compute(value) + 1;
  }
file shared.h: |
  #pragma once

  int compute(int value);
snapshot: |
  use:
    definition:
      - { file: "${WS}/find_references/01_refs_cross_tu/lib.cpp", range: "2:4-2:11" }
    declaration:
      - { file: "${WS}/find_references/01_refs_cross_tu/lib.cpp", range: "2:4-2:11" }
      - { file: "${WS}/find_references/01_refs_cross_tu/shared.h", range: "2:4-2:11" }
    references:
      - { file: "${WS}/find_references/01_refs_cross_tu/lib.cpp", range: "2:4-2:11" }
      - { file: "${WS}/find_references/01_refs_cross_tu/lib.cpp", range: "7:11-7:18" }
      - { file: "${WS}/find_references/01_refs_cross_tu/main.cpp", range: "13:11-13:18" }
      - { file: "${WS}/find_references/01_refs_cross_tu/shared.h", range: "2:4-2:11" }
    callHierarchy:
      - { name: "compute", kind: Function, file: "${WS}/find_references/01_refs_cross_tu/lib.cpp", range: "2:4-2:11" }
    incomingCalls:
      - { name: "again", kind: Function, file: "${WS}/find_references/01_refs_cross_tu/lib.cpp", range: "6:4-6:9", fromRanges: ["7:11-7:25"] }
      - { name: "run", kind: Function, file: "${WS}/find_references/01_refs_cross_tu/main.cpp", range: "12:4-12:7", fromRanges: ["13:11-13:25"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration and definition sites appear among references**

A reference query returns the declaration and the out-of-line
definition together with every use, so the whole surface of a symbol
is reachable from any one of its sites.

```snap-navigation
feature: navigation
code: |
  int §(decl)scale(int value);

  int §(def)scale(int value) {
      return value * 2;
  }

  int use() {
      return §(use)scale(3);
  }
snapshot: |
  decl:
    definition:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "9:4-9:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "16:11-16:16" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "15:4-15:7", fromRanges: ["16:11-16:19"] }

  def:
    definition:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "9:4-9:9" }
    declaration:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "9:4-9:9" }
    references:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "9:4-9:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "16:11-16:16" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "15:4-15:7", fromRanges: ["16:11-16:19"] }

  use:
    definition:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    declaration:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "9:4-9:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    references:
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "9:4-9:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
      - { file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "16:11-16:16" }
    callHierarchy:
      - { name: "scale", kind: Function, file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "11:4-11:9" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/find_references/02_refs_include_declaration.cpp", range: "15:4-15:7", fromRanges: ["16:11-16:19"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1081 -->

**Implicit references from range-based for loops**

Find references on `begin` reports only its own declaration; the
range-based for loop that implicitly calls it is not included among the
references.

```snap-navigation
feature: navigation
code: |
  struct Iterator {
      int operator*() const;
      Iterator& operator++();
      bool operator!=(const Iterator& other) const;
  };

  struct Range {
      Iterator begin();  // find-refs here omits the range-for below
      Iterator end();
  };

  void use(Range r) {
      for (int x : r) {
      }
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Implicit constructor and destructor calls**

Find references on a constructor reports only its explicit sites; an
object definition that implicitly invokes the constructor or its
destructor is not included.

```snap-navigation
feature: navigation
code: |
  struct Blob {
      Blob();  // find-refs here omits the `Blob b;` definition below
      ~Blob();
  };

  void use() {
      Blob b;
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 clangd#1872 -->

**References through forwarding functions**

Find references on a constructor does not include call sites that reach
it indirectly through a perfect-forwarding factory.

```snap-navigation
feature: navigation
code: |
  template <typename T, typename... Args>
  T make(Args&&... args) {
      return T(static_cast<Args&&>(args)...);
  }

  struct Widget {
      Widget(int w, int h);  // find-refs here omits the make<Widget> call
  };

  Widget build() {
      return make<Widget>(800, 600);
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#258 clangd#675 -->

**References in dependent and template contexts**

Find references on a member does not include dependent call sites in a
template, even when the template is instantiated with the member's
class.

```snap-navigation
feature: navigation
code: |
  struct A {
      void foo();  // find-refs here omits the dependent obj.foo() below
  };

  template <typename T>
  void process(T& obj) {
      obj.foo();
  }

  void run(A a) {
      process(a);
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2139 -->

**Read/write classification of references**

The reference reply carries only locations, so a reader cannot tell a
write from a read; annotating each result with its access kind is not
offered.

```snap-navigation
feature: navigation
code: |
  int use() {
      int x = 0;      // write
      int y = x + 1;  // read
      x = y;          // write
      return x;
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#177 -->

**Enclosing function shown with each reference**

Each reference is reported as a bare location; the name of the function
that encloses it is not attached, so results carry no context beyond
the file and line.

```snap-navigation
feature: navigation
code: |
  int shared_value = 0;

  int reader() {
      return shared_value;
  }

  int writer() {
      shared_value = 1;
      return shared_value;
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro references across expansions, `#ifdef`/`#ifndef` and `#undef`**

A macro's references span its expansions, the `#ifdef` / `#ifndef`
conditionals that test it and the `#undef` that cancels it. Each
`#define` of a name is its own symbol, so a redefinition after `#undef`
collects only its own uses.

```snap-navigation
feature: navigation
code: |
  #define §(first)FEATURE 1

  int on = FEATURE;

  #ifdef FEATURE
  int guarded = 1;
  #endif

  #ifndef FEATURE
  int missing = 0;
  #endif

  #undef FEATURE

  #define §(second)FEATURE 2

  int again = FEATURE;
snapshot: |
  first:
    definition:
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "10:8-10:15" }
    declaration:
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "10:8-10:15" }
    references:
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "10:8-10:15" }
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "12:9-12:16" }
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "14:7-14:14" }
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "18:8-18:15" }
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "22:7-22:14" }

  second:
    definition:
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "24:8-24:15" }
    declaration:
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "24:8-24:15" }
    references:
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "24:8-24:15" }
      - { file: "${WS}/find_references/09_refs_macro.cpp", range: "26:12-26:19" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#346 -->

**Macro references spelled inside other macro definitions**

Find references on a macro does not include the mentions of it written
inside the bodies of other macro definitions.

```snap-navigation
feature: navigation
code: |
  #define WIDTH 100  // find-refs here omits the WIDTH tokens in AREA below

  #define AREA (WIDTH * WIDTH)

  int total = AREA;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Label and goto references**

Find references on a label lists the label itself together with every
`goto` that jumps to it.

```snap-navigation
feature: navigation
code: |
  int loop(int failed) {
      §(label)retry:
      if (failed) {
          goto retry;
      }
      return 0;
  }
snapshot: |
  label:
    definition:
      - { file: "${WS}/find_references/11_refs_label_goto.cpp", range: "9:4-9:9" }
    declaration:
      - { file: "${WS}/find_references/11_refs_label_goto.cpp", range: "9:4-9:9" }
    references:
      - { file: "${WS}/find_references/11_refs_label_goto.cpp", range: "9:4-9:9" }
      - { file: "${WS}/find_references/11_refs_label_goto.cpp", range: "11:13-11:18" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Call Hierarchy

<!-- BEGIN GENERATED ITEMS: call_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Prepare call hierarchy on functions and methods**

Preparing a call hierarchy works on a free function and on a member
method alike, anchoring an item at the entity under the cursor.

```snap-navigation
feature: navigation
code: |
  struct Service {
      void §(method)start();
  };

  void Service::start() {}

  void §(func)launch(Service& s) {
      s.start();
  }
snapshot: |
  func:
    definition:
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "14:5-14:11" }
    declaration:
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "14:5-14:11" }
    references:
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "14:5-14:11" }
    callHierarchy:
      - { name: "launch", kind: Function, file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "14:5-14:11" }
    outgoingCalls:
      - { name: "start", kind: Method, file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "12:14-12:19", fromRanges: ["15:4-15:13"] }

  method:
    definition:
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "12:14-12:19" }
    declaration:
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "12:14-12:19" }
    references:
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "9:9-9:14" }
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "12:14-12:19" }
      - { file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "15:6-15:11" }
    callHierarchy:
      - { name: "start", kind: Method, file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "12:14-12:19" }
    incomingCalls:
      - { name: "launch", kind: Function, file: "${WS}/call_hierarchy/01_calls_prepare.cpp", range: "14:5-14:11", fromRanges: ["15:4-15:13"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Incoming calls**

Incoming calls list every caller of a function, and a caller that
invokes it more than once contributes each call site.

```snap-navigation
feature: navigation
code: |
  int §(target)helper(int v) {
      return v;
  }

  int alpha() {
      return helper(1);
  }

  int beta() {
      return helper(2) + helper(3);
  }
snapshot: |
  target:
    definition:
      - { file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "8:4-8:10" }
    declaration:
      - { file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "8:4-8:10" }
    references:
      - { file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "8:4-8:10" }
      - { file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "13:11-13:17" }
      - { file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "17:11-17:17" }
      - { file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "17:23-17:29" }
    callHierarchy:
      - { name: "helper", kind: Function, file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "8:4-8:10" }
    incomingCalls:
      - { name: "alpha", kind: Function, file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "12:4-12:9", fromRanges: ["13:11-13:20"] }
      - { name: "beta", kind: Function, file: "${WS}/call_hierarchy/02_calls_incoming.cpp", range: "16:4-16:8", fromRanges: ["17:11-17:20", "17:23-17:32"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Outgoing calls**

Outgoing calls list every function a body invokes, one entry per
callee.

```snap-navigation
feature: navigation
code: |
  int one() {
      return 1;
  }

  int two() {
      return 2;
  }

  int three() {
      return 3;
  }

  int §(caller)dispatch() {
      return one() + two() + three();
  }
snapshot: |
  caller:
    definition:
      - { file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "20:4-20:12" }
    declaration:
      - { file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "20:4-20:12" }
    references:
      - { file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "20:4-20:12" }
    callHierarchy:
      - { name: "dispatch", kind: Function, file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "20:4-20:12" }
    outgoingCalls:
      - { name: "one", kind: Function, file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "8:4-8:7", fromRanges: ["21:11-21:16"] }
      - { name: "two", kind: Function, file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "12:4-12:7", fromRanges: ["21:19-21:24"] }
      - { name: "three", kind: Function, file: "${WS}/call_hierarchy/03_calls_outgoing.cpp", range: "16:4-16:9", fromRanges: ["21:27-21:34"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Function signature in the item detail**

A call hierarchy item carries only its name; the function signature is
not attached in a detail field, so overloads are indistinguishable in
the hierarchy.

```snap-navigation
feature: navigation
code: |
  int compute(int a, int b) {  // no signature attached to this item
      return a + b;
  }

  int caller() {
      return compute(1, 2);
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Qualified name for member functions**

A member function's call hierarchy item is produced, but its name field
carries only the bare method name (`draw`), not the qualified
`Circle::draw` that would tell it apart from a free function.

```snap-navigation
feature: navigation
code: |
  struct Circle {
      void §(method)draw();
  };

  void Circle::draw() {}
snapshot: |
  method:
    definition:
      - { file: "${WS}/call_hierarchy/05_calls_qualified_name.cpp", range: "13:13-13:17" }
    declaration:
      - { file: "${WS}/call_hierarchy/05_calls_qualified_name.cpp", range: "13:13-13:17" }
    references:
      - { file: "${WS}/call_hierarchy/05_calls_qualified_name.cpp", range: "10:9-10:13" }
      - { file: "${WS}/call_hierarchy/05_calls_qualified_name.cpp", range: "13:13-13:17" }
    callHierarchy:
      - { name: "draw", kind: Method, file: "${WS}/call_hierarchy/05_calls_qualified_name.cpp", range: "13:13-13:17" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Follow virtual dispatch**

Incoming calls of a base virtual method do not include calls made
through derived overrides; a call to an override is attributed only to
that override, never to the base it overrides.

```snap-navigation
feature: navigation
code: |
  struct Base {
      virtual void draw();
  };

  struct Derived : Base {
      void draw() override;
  };

  void call_derived(Derived& d) {
      d.draw();  // absent from the incoming calls of Base::draw
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1308 -->

**Non-function targets**

Variables and enum constants

Preparing a call hierarchy on a variable or an enum constant returns
nothing; the request is offered only for functions and methods.

```snap-navigation
feature: navigation
code: |
  int counter = 0;  // prepare call hierarchy here → nothing

  enum Mode {
      Fast,  // prepare call hierarchy here → nothing
      Slow,
  };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Calls inside lambdas**

A call written in a lambda body appears in the incoming calls of the
function it invokes, attributed to the function that encloses the
lambda.

```snap-navigation
feature: navigation
code: |
  void §(foo)foo() {}

  void use() {
      auto task = [] {
          foo();
      };
      task();
  }
snapshot: |
  foo:
    definition:
      - { file: "${WS}/call_hierarchy/08_calls_lambda.cpp", range: "9:5-9:8" }
    declaration:
      - { file: "${WS}/call_hierarchy/08_calls_lambda.cpp", range: "9:5-9:8" }
    references:
      - { file: "${WS}/call_hierarchy/08_calls_lambda.cpp", range: "9:5-9:8" }
      - { file: "${WS}/call_hierarchy/08_calls_lambda.cpp", range: "13:8-13:11" }
    callHierarchy:
      - { name: "foo", kind: Function, file: "${WS}/call_hierarchy/08_calls_lambda.cpp", range: "9:5-9:8" }
    incomingCalls:
      - { name: "use", kind: Function, file: "${WS}/call_hierarchy/08_calls_lambda.cpp", range: "11:5-11:8", fromRanges: ["13:8-13:13"] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2242 -->

**Constructor calls through forwarding functions**

Incoming calls of a constructor do not include the call sites that
reach it through a perfect-forwarding factory.

```snap-navigation
feature: navigation
code: |
  template <typename T, typename... Args>
  T make(Args&&... args) {
      return T(static_cast<Args&&>(args)...);
  }

  struct Widget {
      Widget(int w, int h);  // make<Widget> below is absent from incoming calls
  };

  Widget build() {
      return make<Widget>(800, 600);
  }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Hierarchy

<!-- BEGIN GENERATED ITEMS: type_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Prepare type hierarchy on class, struct, enum and union**

Preparing a type hierarchy anchors an item on any user-defined type
tag — class, struct, enum and union alike.

```snap-navigation
feature: navigation
code: |
  class §(cls)Handle {};

  struct §(strct)Point {};

  enum class §(enm)Mode {};

  union §(uni)Storage {
      int i;
      float f;
  };
snapshot: |
  cls:
    definition:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "8:6-8:12" }
    declaration:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "8:6-8:12" }
    references:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "8:6-8:12" }
    typeHierarchy:
      - { name: "Handle", kind: Class, file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "8:6-8:12" }

  enm:
    definition:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "12:11-12:15" }
    declaration:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "12:11-12:15" }
    references:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "12:11-12:15" }
    typeHierarchy:
      - { name: "Mode", kind: Enum, file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "12:11-12:15" }

  strct:
    definition:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "10:7-10:12" }
    declaration:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "10:7-10:12" }
    references:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "10:7-10:12" }
    typeHierarchy:
      - { name: "Point", kind: Struct, file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "10:7-10:12" }

  uni:
    definition:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "14:6-14:13" }
    declaration:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "14:6-14:13" }
    references:
      - { file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "14:6-14:13" }
    typeHierarchy:
      - { name: "Storage", kind: Class, file: "${WS}/type_hierarchy/01_types_prepare.cpp", range: "14:6-14:13" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Supertypes**

Supertypes list every direct base of a class, including each base of a
multiple-inheritance derived type.

```snap-navigation
feature: navigation
code: |
  struct Alpha {};

  struct Beta {};

  struct §(derived)Gamma : Alpha, Beta {};
snapshot: |
  derived:
    definition:
      - { file: "${WS}/type_hierarchy/02_types_supertypes.cpp", range: "12:7-12:12" }
    declaration:
      - { file: "${WS}/type_hierarchy/02_types_supertypes.cpp", range: "12:7-12:12" }
    references:
      - { file: "${WS}/type_hierarchy/02_types_supertypes.cpp", range: "12:7-12:12" }
    typeHierarchy:
      - { name: "Gamma", kind: Struct, file: "${WS}/type_hierarchy/02_types_supertypes.cpp", range: "12:7-12:12" }
    supertypes:
      - { name: "Alpha", kind: Struct, file: "${WS}/type_hierarchy/02_types_supertypes.cpp", range: "8:7-8:12" }
      - { name: "Beta", kind: Struct, file: "${WS}/type_hierarchy/02_types_supertypes.cpp", range: "10:7-10:11" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Subtypes**

Subtypes list every class that derives from a base, across sibling
derived types.

```snap-navigation
feature: navigation
code: |
  struct §(base)Shape {};

  struct Circle : Shape {};

  struct Square : Shape {};

  struct Triangle : Shape {};
snapshot: |
  base:
    definition:
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "8:7-8:12" }
    declaration:
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "8:7-8:12" }
    implementation:
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "10:7-10:13" }
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "12:7-12:13" }
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "14:7-14:15" }
    references:
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "8:7-8:12" }
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "10:16-10:21" }
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "12:16-12:21" }
      - { file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "14:18-14:23" }
    typeHierarchy:
      - { name: "Shape", kind: Struct, file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "8:7-8:12" }
    subtypes:
      - { name: "Circle", kind: Struct, file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "10:7-10:13" }
      - { name: "Square", kind: Struct, file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "12:7-12:13" }
      - { name: "Triangle", kind: Struct, file: "${WS}/type_hierarchy/03_types_subtypes.cpp", range: "14:7-14:15" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template inheritance**

Subtypes of a base include classes that derive from it through a class
template, such as a CRTP wrapper.

```snap-navigation
feature: navigation
code: |
  struct §(base)Base {};

  template <typename T>
  struct CRTP : Base {};

  struct Widget : CRTP<Widget> {};
snapshot: |
  base:
    definition:
      - { file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "8:7-8:11" }
    declaration:
      - { file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "8:7-8:11" }
    implementation:
      - { file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "11:7-11:11" }
    references:
      - { file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "8:7-8:11" }
      - { file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "11:14-11:18" }
    typeHierarchy:
      - { name: "Base", kind: Struct, file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "8:7-8:11" }
    subtypes:
      - { name: "CRTP", kind: Struct, file: "${WS}/type_hierarchy/04_types_template_inheritance.cpp", range: "11:7-11:11" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#31 -->

**Template arguments in type hierarchy items**

A subtype produced by a class template specialization is listed, but
its item name carries only the bare template name (`Derived`), without
the template arguments that would distinguish `Derived<Foo>`.

```snap-navigation
feature: navigation
code: |
  struct Foo {};

  struct §(base)Base {};

  template <typename T>
  struct Derived : Base {};

  Derived<Foo> instance;
snapshot: |
  base:
    definition:
      - { file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "12:7-12:11" }
    declaration:
      - { file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "12:7-12:11" }
    implementation:
      - { file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "15:7-15:14" }
    references:
      - { file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "12:7-12:11" }
      - { file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "15:17-15:21" }
    typeHierarchy:
      - { name: "Base", kind: Struct, file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "12:7-12:11" }
    subtypes:
      - { name: "Derived", kind: Struct, file: "${WS}/type_hierarchy/05_types_template_args.cpp", range: "15:7-15:14" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Workspace Symbol

Search the whole project for a symbol by name (`workspace/symbol`).

<!-- BEGIN GENERATED ITEMS: workspace_symbol -->

<!-- BEGIN CAPABILITY: supported -->

**Basic workspace-wide symbol search**

case-insensitive substring matching

A query matches any symbol whose name contains it, ignoring case:
functions, types, enumerators and macros all participate, and a query
with no match returns an empty list rather than an error.

```snap-workspace_symbol
feature: workspace_symbol
code: |
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
snapshot: |
  "widget":
    - { name: "Widget", kind: Struct, file: "${WS}/workspace_symbol/01_basic_search.cpp", range: "15:7-15:13" }

  "parse_config":
    - { name: "parse_config", kind: Function, file: "${WS}/workspace_symbol/01_basic_search.cpp", range: "23:5-23:17" }

  "MODE":
    - { name: "Mode", kind: Enum, file: "${WS}/workspace_symbol/01_basic_search.cpp", range: "19:11-19:15" }
    - { name: "MODE_DEFAULT", kind: Function, file: "${WS}/workspace_symbol/01_basic_search.cpp", range: "21:8-21:20" }

  "fast":
    - { name: "Fast", kind: EnumMember, file: "${WS}/workspace_symbol/01_basic_search.cpp", range: "19:18-19:22" }

  "no_such_symbol": none
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Search spans the whole project**

Hits from files other than the queried one

The query returns symbols from project files that are not even open
in the editor: `other.h` stays closed here, so its hit is served by
the background index.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: helper_elsewhere

  int local_anchor = 0;
file other.h: |
  void helper_elsewhere() {}
snapshot: |
  "helper_elsewhere":
    - { name: "helper_elsewhere", kind: Function, file: "${WS}/workspace_symbol/02_cross_file_search/other.h", range: "0:5-0:21" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1344 -->

**Overload disambiguation**

Parameter types shown in results

Querying an overloaded name finds every overload, but each entry
carries only the bare name — nothing tells the two `process` results
apart short of opening both locations.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: process

  void process(int value) {}

  void process(bool flag, int level) {}
snapshot: |
  "process":
    - { name: "process", kind: Function, file: "${WS}/workspace_symbol/03_overload_params.cpp", range: "12:5-12:12" }
    - { name: "process", kind: Function, file: "${WS}/workspace_symbol/03_overload_params.cpp", range: "14:5-14:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#914 -->

**Fuzzy matching**

word-boundary-aware scoring for camelCase and snake_case

Matching is a case-insensitive substring test: `LinLis` does not find
`LinkedList`, and `pcfg` does not find `parse_config`. Word-boundary
initials should match and score for every symbol kind, macros
included.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: LinLis
  // query: pcfg

  struct LinkedList {};

  void parse_config();
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#550 -->

**Partially qualified name search**

Symbols match by bare name only: `net::Socket` finds nothing even
though `deep::net::Socket` exists, and neither does any other
qualifier-prefixed form.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: net::Socket

  namespace deep {
  namespace net {

  struct Socket {};

  }  // namespace net
  }  // namespace deep
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#931 -->

**Enumerator lookup under the enum's scope**

`Color::Red` should find the enumerator — for scoped and unscoped
enums alike — but qualified queries match nothing; only the bare
`Red` does.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: Color::Red

  enum Color { Red, Green };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2253 -->

**Underlying declarations ranked above type aliases**

When both `ConnectionImpl` and its alias `Connection` match a query,
the underlying declaration should rank first. Results carry no
ranking today.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: Connection

  struct ConnectionImpl {};

  using Connection = ConnectionImpl;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Search by mangled (linker) name**

Pasting a linker symbol such as `_Z7processi` should resolve to the
function it mangles — useful when chasing linker errors and stack
traces.

```snap-workspace_symbol
feature: workspace_symbol
code: |
  // query: _Z7processi

  void process(int value);
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Navigation

<!-- BEGIN GENERATED ITEMS: module_navigation -->

<!-- BEGIN CAPABILITY: supported clangd#2310 -->

**`import module_name` navigates to the module interface unit**

Go-to-definition on the name in an `import` declaration opens the
module interface unit that exports it, and uses of an imported symbol
reach its definition in that unit.

```snap-navigation
feature: navigation
code: |
  import §(module_name)widget;

  int build() {
      return §(imported_use)area(2, 3);
  }
file widget.cppm: |
  export module widget;

  export int area(int width, int height) {
      return width * height;
  }
snapshot: |
  imported_use:
    definition:
      - { file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "2:11-2:15" }
    declaration:
      - { file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "2:11-2:15" }
    references:
      - { file: "${WS}/module_navigation/01_module_import_name/main.cpp", range: "13:11-13:15" }
      - { file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "2:11-2:15" }
    callHierarchy:
      - { name: "area", kind: Function, file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "2:11-2:15" }
    incomingCalls:
      - { name: "build", kind: Function, file: "${WS}/module_navigation/01_module_import_name/main.cpp", range: "12:4-12:9", fromRanges: ["13:11-13:21"] }

  module_name:
    definition:
      - { file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "0:14-0:20" }
    declaration:
      - { file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "0:14-0:20" }
    references:
      - { file: "${WS}/module_navigation/01_module_import_name/main.cpp", range: "10:7-10:13" }
      - { file: "${WS}/module_navigation/01_module_import_name/widget.cppm", range: "0:14-0:20" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`import :partition` navigates to the partition unit**

Go-to-definition on the partition name after the colon in a partition
import opens the partition unit that declares it.

```snap-navigation
feature: navigation
code: |
  import pack;

  int run() {
      return §(use)count();
  }
file pack.cppm: |
  export module pack;

  export import :items;
file pack_items.cppm: |
  export module pack:items;

  export int count() {
      return 3;
  }
snapshot: |
  --- main.cpp
  use:
    definition:
      - { file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "2:11-2:16" }
    declaration:
      - { file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "2:11-2:16" }
    references:
      - { file: "${WS}/module_navigation/02_module_partition_import/main.cpp", range: "11:11-11:16" }
      - { file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "2:11-2:16" }
    callHierarchy:
      - { name: "count", kind: Function, file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "2:11-2:16" }
    incomingCalls:
      - { name: "run", kind: Function, file: "${WS}/module_navigation/02_module_partition_import/main.cpp", range: "10:4-10:7", fromRanges: ["11:11-11:18"] }

  --- pack.cppm
  partition_ref:
    definition:
      - { file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "0:14-0:24" }
    declaration:
      - { file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "0:14-0:24" }
    references:
      - { file: "${WS}/module_navigation/02_module_partition_import/pack.cppm", range: "2:14-2:15" }
      - { file: "${WS}/module_navigation/02_module_partition_import/pack_items.cppm", range: "0:14-0:24" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Navigate between interface and implementation units of one module**

Go-to-definition on the module name in an implementation unit
(`module m;`) jumps to the interface unit that declares the module;
the reverse direction, from the interface name to the implementation,
is not offered.

```snap-navigation
feature: navigation
code: |
  import store;

  int lookup(int key) {
      return fetch(key);
  }
file iface.cppm: |
  export module store;

  export int fetch(int key);
file impl.cpp: |
  module store;

  int fetch(int key) {
      return key * 2;
  }
snapshot: |
  --- iface.cppm
  iface_name:
    definition:
      - { file: "${WS}/module_navigation/03_module_iface_impl/iface.cppm", range: "0:14-0:19" }
    declaration:
      - { file: "${WS}/module_navigation/03_module_iface_impl/iface.cppm", range: "0:14-0:19" }
    references:
      - { file: "${WS}/module_navigation/03_module_iface_impl/iface.cppm", range: "0:14-0:19" }
      - { file: "${WS}/module_navigation/03_module_iface_impl/impl.cpp", range: "0:7-0:12" }
      - { file: "${WS}/module_navigation/03_module_iface_impl/main.cpp", range: "10:7-10:12" }

  --- impl.cpp
  impl_name:
    definition:
      - { file: "${WS}/module_navigation/03_module_iface_impl/iface.cppm", range: "0:14-0:19" }
    declaration:
      - { file: "${WS}/module_navigation/03_module_iface_impl/iface.cppm", range: "0:14-0:19" }
    references:
      - { file: "${WS}/module_navigation/03_module_iface_impl/iface.cppm", range: "0:14-0:19" }
      - { file: "${WS}/module_navigation/03_module_iface_impl/impl.cpp", range: "0:7-0:12" }
      - { file: "${WS}/module_navigation/03_module_iface_impl/main.cpp", range: "10:7-10:12" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Dot-separated module name**

Navigate each segment

Go-to-definition on the leading segment of a dot-separated module name
reaches the module's interface unit; the segments after a dot do not
resolve on their own yet.

```snap-navigation
feature: navigation
code: |
  import §(seg_app)app.§(seg_core)core;

  int run() {
      return value();
  }
file app_core.cppm: |
  export module app.core;

  export int value() {
      return 1;
  }
snapshot: |
  seg_app:
    definition:
      - { file: "${WS}/module_navigation/04_module_dotted/app_core.cppm", range: "0:14-0:22" }
    declaration:
      - { file: "${WS}/module_navigation/04_module_dotted/app_core.cppm", range: "0:14-0:22" }
    references:
      - { file: "${WS}/module_navigation/04_module_dotted/app_core.cppm", range: "0:14-0:22" }
      - { file: "${WS}/module_navigation/04_module_dotted/main.cpp", range: "9:7-9:10" }

  seg_core: none
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Document Highlight

Highlight all references to the symbol under cursor within the current file (`textDocument/documentHighlight`).

<!-- BEGIN GENERATED ITEMS: document_highlight -->

<!-- BEGIN CAPABILITY: unsupported -->

**Highlight every reference to the symbol under the cursor in the current file**

Placing the cursor on `total` should light up its declaration and
every use in the file; the request is not implemented.

```snap-navigation
feature: navigation
code: |
  int total = 0;

  void accumulate(int amount) {
      total = total + amount;
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Read/write classification for symbol highlights**

Each highlight should carry its access kind, so editors can tint
writes differently from reads.

```snap-navigation
feature: navigation
code: |
  void tally() {
      int count = 0;      // write
      int next = count;   // read
      count = next;       // write
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**Control flow token highlighting**

Highlighting `break` or `continue` should also light up the loop or
`switch` it belongs to — and `return` / `throw` the function exits
they mark.

```snap-navigation
feature: navigation
code: |
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

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Switch Source/Header

<!-- BEGIN GENERATED ITEMS: switch_source_header -->

<!-- BEGIN CAPABILITY: unsupported -->

**Switch between a source file and its header**

From `widget.cpp` a single command should jump to `widget.h` and
back — the `textDocument/switchSourceHeader` request clangd clients
rely on is not implemented.

```snap-navigation
feature: navigation
code: |
  // widget.h
  class Widget {
      void draw();
  };

  // widget.cpp — #include "widget.h"
  void Widget::draw() {}
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
