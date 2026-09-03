# Hover

Rich information cards for the symbol under the cursor.

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Symbol Information

<!-- BEGIN GENERATED ITEMS: symbol_information -->

| Capability                | Status    | Issues                                                      |
| ------------------------- | --------- | ----------------------------------------------------------- |
| Qualified name            | Supported |                                                             |
| Symbol kind               | Supported |                                                             |
| Access specifier          | Supported |                                                             |
| Definition rendering      | Supported |                                                             |
| Initializer truncation    | Partial   | [clangd#710](https://github.com/clangd/clangd/issues/710)   |
| Virtual modifiers         | Partial   | [clangd#2474](https://github.com/clangd/clangd/issues/2474) |
| Anonymous namespace scope | Partial   | [clangd#436](https://github.com/clangd/clangd/issues/436)   |

### Qualified name

The hover card shows the enclosing namespace and class scope

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

### Symbol kind

The card names what the symbol is: struct, enum, function, field, …

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

### Access specifier

Members show their public / protected / private access

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

### Definition rendering

The card includes the symbol's source definition

```cpp
namespace retry {

constexpr int max_retries = 3;

int backoff(int attempt = 1) {
    return attempt * max_retries;
}

}
```

### Initializer truncation

Huge initializers render truncated, not in full

The rendered definition omits the initializer, but the evaluated
`Value` field still spells out all 256 elements.

```cpp
#define A(x) x, x, x, x
#define B(x) A(A(A(A(x))))
int arr[] = {B(0)};
```

### Virtual modifiers

`virtual` / `override` / `final` show on method hover

Modifiers written in the source render (`virtual … = 0`, `override`,
`final`), but an overriding method that omits the redundant `virtual`
keyword gives no sign of its virtuality — the card lacks the
`virtual void draw() override` form the issue asks for.

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

### Anonymous namespace scope

`(anonymous namespace)` shows in the scope display

The cards render, but the anonymous segment is dropped from the
scope display: a top-level anonymous member shows no scope line at
all, and `outer::(anonymous)` shows just `outer`.

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

<!-- END GENERATED ITEMS -->

## Type Information

<!-- BEGIN GENERATED ITEMS: type_information -->

| Capability               | Status      | Issues                                                      |
| ------------------------ | ----------- | ----------------------------------------------------------- |
| Variable types           | Supported   |                                                             |
| Type aliases             | Supported   |                                                             |
| Function signatures      | Supported   |                                                             |
| Template parameters      | Supported   |                                                             |
| `auto` deduction         | Supported   |                                                             |
| `decltype` deduction     | Supported   |                                                             |
| CTAD                     | Partial     | [clangd#435](https://github.com/clangd/clangd/issues/435)   |
| Instantiation arguments  | Partial     | [clangd#230](https://github.com/clangd/clangd/issues/230)   |
| Lambda `auto` parameters | Unsupported | [clangd#493](https://github.com/clangd/clangd/issues/493)   |
| Sugared `auto`           | Supported   |                                                             |
| Type formatting          | Unsupported | [clangd#2156](https://github.com/clangd/clangd/issues/2156) |
| Anonymous struct typedef | Supported   | [clangd#2219](https://github.com/clangd/clangd/issues/2219) |
| Concept constraints      | Partial     |                                                             |

### Variable types

pointers, references, arrays

A variable's card pretty-prints its declared type, spelling the pointer,
reference and array declarators the way they read in source.

```cpp
namespace variable_type {

int target;

int *ptr = &target;

int &ref = target;

int numbers[4]{};

}
```

### Type aliases

The desugared `aka` form

A sugared type shows its underlying type as `Alias (aka int)`. The
`show_aka` option turns the `aka` suffix off.

```cpp
namespace aka_desugar {

using Handle = int;
using Alias = Handle;

Handle direct = 0;

Alias chained = 0;

}
```

### Function signatures

Return type, parameter names, defaults

A function's card lists its return type, each parameter with its name,
and any default argument.

```cpp
namespace function_signature {

int add(int lhs, int rhs);

void configure(int width, bool visible = true);

}
```

### Template parameters

type, template-template, non-type

Each template parameter kind reports its form: a type parameter, a
template-template parameter, and a non-type parameter with its default.

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

### `auto` deduction

The type the placeholder resolves to

Hovering an `auto` placeholder shows the type substituted for it —
builtins, pointers, lambdas, template instantiations, and the
`/* not deduced */` marker inside an uninstantiated template.

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

### `decltype` deduction

value, reference and dependent forms

Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
type, including the reference the parenthesized-expression rule adds.

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

### CTAD

Deduced template arguments of a class placeholder

With class template argument deduction the variable's card shows the
deduced `Box<int>`, but hovering the class-name spelling still reports
the primary template without its arguments.

```cpp
namespace ctad_arguments {

template <typename T> struct Box {
  Box(T);
};

Box picked(42);

}
```

### Instantiation arguments

Template parameters bound at a use site

A use of a template shows the substituted types (`Wrapper<int>`,
`identity<int>`, `int x`), but not an explicit `T = int` mapping of each
parameter to the argument it was bound to.

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

### Lambda `auto` parameters

Deduced parameter type

Hovering the `auto` parameter of a generic lambda yields no card; the
deduced parameter type is not shown.

```cpp
namespace lambda_auto_params {

auto printer = [](auto value) { return value; };

}
```

### Sugared `auto`

Alias sugar preserved through deduction

clangd tracks lost alias sugar through `auto` as clangd#709; clice
already keeps the alias spelling and appends its desugared form, so
`auto` deduced from an aliased return type reads as `Outer // aka: int`.

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

### Type formatting

clang-format applied to rendered types

Long or nested types are printed by the compiler's default type printer;
they are not re-wrapped or aligned through clang-format.

```cpp
namespace clang_format_types {

template <typename A, typename B, typename C, typename D>
struct Tuple {};

Tuple<int, long, unsigned, char> wide;

}
```

### Anonymous struct typedef

The classic C `typedef struct {…} Name`

Compiled as C11: clangd renders a misleading `struct Point` for the
alias of an anonymous struct; clice names the struct after its typedef,
so both the alias and a variable of it report a clean `Point` card.

```cpp
/// A 2-D point.
typedef struct {
  int x, y;
} Point;

Point origin = {.y = 2, .x = 1};
```

### Concept constraints

The constraint behind a parameter or `auto` placeholder

The constrained-parameter and concept-reference cards carry the
constraint, but hovering the placeholder of a constrained `Addable auto`
variable shows only the deduced type — the constraint is dropped.

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

<!-- END GENERATED ITEMS -->

## Layout Information

<!-- BEGIN GENERATED ITEMS: layout_information -->

| Capability        | Status    | Issues                                                      |
| ----------------- | --------- | ----------------------------------------------------------- |
| Field layout      | Supported |                                                             |
| Type-level layout | Partial   | [clangd#1763](https://github.com/clangd/clangd/issues/1763) |
| Vtable offset     | Partial   | [clangd#1771](https://github.com/clangd/clangd/issues/1771) |

### Field layout

size, offset, alignment and padding show on field hover

The corpus pins an x86-64 target, so the bit numbers are stable.

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

### Type-level layout

Hovering the type itself shows its size, alignment and padding

Size and alignment show on the type card today; the total padding
does not yet.

```cpp
namespace layout {

struct Widget {
    int id;
    double value;
};

}
```

### Vtable offset

Virtual methods show their table slot

The method card renders without any vtable fact today.

```cpp
struct Shape {
    virtual void draw();
    virtual void move();
};
```

<!-- END GENERATED ITEMS -->

## Expression Context

<!-- BEGIN GENERATED ITEMS: expression_context -->

| Capability           | Status      | Issues                                                      |
| -------------------- | ----------- | ----------------------------------------------------------- |
| Constant evaluation  | Supported   |                                                             |
| Call arguments       | Supported   |                                                             |
| Pass semantics       | Supported   |                                                             |
| Implicit conversions | Supported   |                                                             |
| String literals      | Partial     | [clangd#1016](https://github.com/clangd/clangd/issues/1016) |
| Numeric literals     | Unsupported | [clangd#1669](https://github.com/clangd/clangd/issues/1669) |
| Record variables     | Partial     | [clangd#1622](https://github.com/clangd/clangd/issues/1622) |

### Constant evaluation

constexpr, enumerators, sizeof

When an initializer is a constant expression, the card evaluates it and
shows the resulting value.

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

### Call arguments

Which parameter each argument binds to

Hovering an argument at a call site shows the parameter it is passed to,
naming the parameter it binds.

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

### Pass semantics

By value, by reference, by const reference

The argument card states how the value reaches the callee: copied by
value, or bound to a mutable or const reference parameter.

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

### Implicit conversions

Argument converted to the parameter type

When an argument reaches a parameter through an implicit conversion, the
card notes the target type, for both built-in and user-defined
conversions.

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

### String literals

The length reported on hover

A string-literal card reports the array type and its size in bytes
(`const char[6]`, `Size: 6 bytes` — the length plus the null
terminator), not an explicit character count.

```cpp
namespace string_length {

const char *greeting = "hello";

}
```

### Numeric literals

Type and value of an integer or float literal

Hovering a numeric literal yields no card, unlike character and string
literals, whose type and value are shown.

```cpp
namespace numeric_literal_type {

auto count = 42;
auto ratio = 3.14;

}
```

### Record variables

Enclosing constant value leaks in

Hovering a record-typed argument of a constant-evaluable call currently
reports that call's value (`Value = 7`) on the variable — a value that
is not the record's own.

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

<!-- END GENERATED ITEMS -->

## Documentation

<!-- BEGIN GENERATED ITEMS: documentation -->

| Capability                         | Status      | Issues                                                      |
| ---------------------------------- | ----------- | ----------------------------------------------------------- |
| Doxygen `///` comments             | Supported   |                                                             |
| Synthesized accessor docs          | Supported   |                                                             |
| `@copydoc` tags                    | Partial     | [clangd#1320](https://github.com/clangd/clangd/issues/1320) |
| Inherited override docs            | Partial     | [clangd#2504](https://github.com/clangd/clangd/issues/2504) |
| Overload doc sharing               | Partial     | [clangd#2506](https://github.com/clangd/clangd/issues/2506) |
| Inherited constructor docs         | Unsupported | [clangd#1936](https://github.com/clangd/clangd/issues/1936) |
| Banner comments                    | Partial     | [clangd#974](https://github.com/clangd/clangd/issues/974)   |
| Declaration vs definition comments | Supported   |                                                             |
| Whitespace and newlines            | Partial     | [clangd#2057](https://github.com/clangd/clangd/issues/2057) |
| Comment indentation                | Partial     | [clangd#1040](https://github.com/clangd/clangd/issues/1040) |
| Template keyword from a macro      | Partial     | [clangd#1226](https://github.com/clangd/clangd/issues/1226) |
| Comment suppression option         | Unsupported | [clangd#2148](https://github.com/clangd/clangd/issues/2148) |

### Doxygen `///` comments

Extracted from the declaration and rendered on hover

Applies to plain functions, primary templates and their specializations;
a reference resolves to the most specialized declaration's comment.

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

### Synthesized accessor docs

Trivial getters/setters get a generated one-line description

A trivial getter or setter with no comment of its own gets a synthesized
"Trivial accessor/setter for `field`." line in its hover card.

```cpp
namespace accessors {
struct Widget {
    int width;
    int getWidth() { return width; }
    void setWidth(int w) { width = w; }
};
}
```

### `@copydoc` tags

Copy another symbol's documentation onto this one

A `@copydoc target` tag should copy `target`'s documentation into this
symbol's hover card. clice does not resolve the tag yet — the card shows
the literal `@copydoc base_func()` text.

```cpp
namespace copydoc {
/// Detailed documentation.
void base_func();

/// @copydoc base_func()
void wrapper();
}
```

### Inherited override docs

An override with no comment shows the base method's documentation

Hovering an overriding method that carries no comment of its own should
surface the documentation from the method it overrides. clice does not
inherit it yet — the override's card carries no description.

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

### Overload doc sharing

A later overload with no comment reuses the first overload's documentation

Consecutive overloads often document only the first; a later undocumented
overload should reuse that shared description. clice does not share it
yet — the later overload's card carries no description.

```cpp
namespace overloads {
/// Opens a file.
void open(const char* path);
void open(const char* path, int flags);
}
```

### Inherited constructor docs

`using Base::Base;` surfaces the base constructor's documentation

A constructor pulled in with `using Base::Base;` should carry the base
constructor's documentation on hover. There is no hover surface for it:
the name in the using-declaration resolves to the class, not the
inherited constructor.

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

### Banner comments

A section banner separated by a blank line must not attach to the next declaration

A `// ==== Section ====` banner followed by a blank line should not be
misattributed as documentation for the declaration below it. clice
currently attaches it anyway — the banner text appears in the card.

```cpp
namespace banners {
// ==== Section Banner ====

void foo();
}
```

### Declaration vs definition comments

The declaration's doc wins over a definition-site comment

clangd tracks this as clangd#829; clice already prefers the
declaration's `///` documentation over the definition's plain `//` note,
showing it at both the declaration and the definition site.

```cpp
namespace decldef {
/// Public API documentation.
void process(int x);

// Internal implementation note.
void process(int x) { (void)x; }
}
```

### Whitespace and newlines

A markdown table in a comment keeps its line breaks

A markdown table written across several `///` lines should render as a
table with its line breaks preserved. clice currently flattens the lines
onto one line, so the table does not render.

```cpp
namespace tables {
/// | Column A | Column B |
/// |----------|----------|
/// | 1        | 2        |
void table_fn();
}
```

### Comment indentation

Indented lines in a comment render without spurious extra indentation

A doc comment whose body contains an indented block should render with
correct indentation. clice currently strips the leading indentation, so
an indented code block loses its offset and the blank line collapses.

```cpp
namespace indented {
/// Summary line.
///
///     step_one();
///     step_two();
void run();
}
```

### Template keyword from a macro

The docstring should survive the expansion

When the `template` keyword is produced by a macro expansion, the
declaration's doc comment should still appear on hover. clice currently
drops it — the card carries no description.

```cpp
int anchor = 0;

#define TEMPLATE template

/// A documented template function.
TEMPLATE <typename T> void run(T value);
```

### Comment suppression option

A config switch to hide misattributed doc comments

A stray comment picked up by the association heuristic — a section
banner separated from the code by a blank line, for example — always
reaches the hover card: clice has no config option to suppress doc
comments whose attachment is a guess.

```cpp
namespace suppression {
// TODO: tidy this file up.

int counter;
}
```

<!-- END GENERATED ITEMS -->

## Macro Hover

<!-- BEGIN GENERATED ITEMS: macro_hover -->

| Capability                    | Status      | Issues                                                      |
| ----------------------------- | ----------- | ----------------------------------------------------------- |
| Definition text at every site | Supported   |                                                             |
| Fully-expanded preview        | Supported   |                                                             |
| Command-line macros           | Supported   |                                                             |
| Nested macro in arguments     | Partial     |                                                             |
| Use before definition         | Partial     | [clangd#2642](https://github.com/clangd/clangd/issues/2642) |
| `#define` inside the preamble | Unsupported |                                                             |

### Definition text at every site

`#define`, use, `#ifdef` and `#undef` all show the macro's definition

A macro's hover card carries its `#define` text wherever the name
appears: the definition itself, a use, an `#ifdef` guard and an `#undef`.

```cpp
int anchor = 0;

#define LIMIT 64

int use = LIMIT;

#ifdef LIMIT
int guarded = 1;
#endif

#undef LIMIT
```

### Fully-expanded preview

A function-like macro use shows its arguments substituted through the body

Hovering a function-like macro invocation shows the `#define` text and a
preview of the fully-expanded result with the call's arguments spliced in.

```cpp
int x = 1, y = 2;

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int z = MAX(x, y);
```

### Command-line macros

`-D` definitions hover with a synthesized `#define`

A macro defined on the command line (`-DFROM_CLI=7`) shows a synthesized
`#define FROM_CLI 7` in its hover card, then its expansion.

```cpp
int cli = FROM_CLI;
```

### Nested macro in arguments

A macro named inside another invocation's arguments

The recorded expansion starts at the outer invocation, so hovering an
inner macro named inside the arguments shows only its definition, not an
expansion preview.

```cpp
int anchor = 0;

#define ECHO(x) x
#define INNER_VAL 99

int nested = ECHO(INNER_VAL);
```

### Use before definition

Hovering a macro name that appears before its `#define`

A macro name used in an `#if` above its own `#define` should still hover
with the macro's definition. clice currently returns no hover at the
pre-definition use; a use after the `#define` works normally.

```cpp
int anchor = 0;

#if COUNT > 0
int positive = 1;
#endif

#define COUNT 3

int use = COUNT;
```

### `#define` inside the preamble

Hover on a leading directive

A `#define` in the file's preamble region (the leading run of directives
before the first declaration) is not part of the live parse's
preprocessor record, so hovering its name yields nothing. Every other
macro fixture opens with a declaration precisely to push its directives
past the preamble boundary.

```cpp
#define EARLY 1

int use = EARLY;
```

<!-- END GENERATED ITEMS -->

## Special Hover Targets

<!-- BEGIN GENERATED ITEMS: special_hover_targets -->

| Capability                     | Status      | Issues                                                      |
| ------------------------------ | ----------- | ----------------------------------------------------------- |
| Members on type hover          | Partial     | [clangd#959](https://github.com/clangd/clangd/issues/959)   |
| Typedef underlying struct      | Partial     | [clangd#2020](https://github.com/clangd/clangd/issues/2020) |
| Keyword documentation          | Unsupported | [clangd#1862](https://github.com/clangd/clangd/issues/1862) |
| Attribute documentation        | Supported   | [clangd#1862](https://github.com/clangd/clangd/issues/1862) |
| Include directive hover        | Supported   |                                                             |
| `this` expression              | Supported   |                                                             |
| Predefined identifiers         | Supported   |                                                             |
| No hover on meaningless tokens | Supported   |                                                             |
| GTK-Doc and kernel-doc         | Unsupported | [clangd#2662](https://github.com/clangd/clangd/issues/2662) |
| LaTeX math in Doxygen          | Unsupported | [clangd#2669](https://github.com/clangd/clangd/issues/2669) |

### Members on type hover

Hovering an enum or struct type lists its members

The card names the type (and a struct's layout), but the member list is
not expanded — the body renders as `{}`.

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

### Typedef underlying struct

Hovering an alias expands the aliased definition

The card resolves the alias to its underlying type name, but does not
expand that struct's definition or member list.

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

### Keyword documentation

Hovering a language keyword shows its description

Hovering a keyword such as `const` or `virtual` produces no card.

```cpp
namespace keywords {

const int limit = 42;

struct Widget {
    virtual void draw();
};

}
```

### Attribute documentation

Hovering an attribute shows its description

The attribute's own documentation renders in the card, for both GNU
`__attribute__` spellings and C++ `[[...]]` attributes.

```cpp
namespace attr_docs {
void foo(int * __attribute__((nonnull, noescape)) );

[[nodiscard]] int compute();
}
```

### Include directive hover

Hovering an `#include` shows the resolved header path

The card resolves the quoted header to its file on disk.

```cpp
#include "own_header.h"

int use = own_header_value;
```

### `this` expression

Hovering `this` shows the pointed-to class type

Works in a plain class and inside a class template.

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

### Predefined identifiers

`__func__` hover shows the current function name

The value resolves in a concrete function; inside a template only the
approximate type is known.

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

### No hover on meaningless tokens

Builtin keywords and empty bodies yield no card

Hovering a builtin type keyword or the inside of an empty body
produces no card at all, so editors show nothing rather than noise.
(Numeric and bool literals also have no card today, but that is a
tracked gap — see the numeric-literal item — not a promise.)

```cpp
namespace negatives {

int counter = 0;

void noop() {}

}
```

### GTK-Doc and kernel-doc

Recognize GObject Introspection annotations

GTK-Doc / kernel-doc comment syntax and GObject Introspection
annotations are not parsed into the hover card.

```cpp
/**
 * gtk_widget_show:
 * @widget: (transfer none): a #GtkWidget
 *
 * Flags a widget to be displayed.
 */
void gtk_widget_show(GtkWidget *widget);
```

### LaTeX math in Doxygen

Render `@f$ ... @f$` formulas

Doxygen LaTeX math formulas are shown verbatim, not rendered as math.

```cpp
/// The area of a circle is @f$ A = \pi r^2 @f$.
double circle_area(double r);
```

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: presentation -->

| Capability         | Status    | Issues |
| ------------------ | --------- | ------ |
| Markdown rendering | Supported |        |

### Markdown rendering

Cards render as markdown, or plain text via `parse_comment_as_markdown = false`

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

<!-- END GENERATED ITEMS -->

## Module-Related

<!-- BEGIN GENERATED ITEMS: module_related -->

| Capability             | Status      | Issues |
| ---------------------- | ----------- | ------ |
| Import statement hover | Unsupported |        |
| Module name hover      | Unsupported |        |

### Import statement hover

Hovering `import` shows the module's info

Hovering an `import` declaration does not yet describe the imported
module.

```cpp
export module app;

import utils;
```

### Module name hover

Hovering a module name lists its owning files

Hovering a module name does not yet list the files or partitions that
declare it.

```cpp
export module math;

export module math:algebra;
```

<!-- END GENERATED ITEMS -->

## Hover Correctness

Robustness on inputs that have broken other tooling.

<!-- BEGIN GENERATED ITEMS: hover_correctness -->

| Capability                   | Status    | Issues |
| ---------------------------- | --------- | ------ |
| MSVC inheritance model       | Supported |        |
| Most-vexing-parse            | Supported |        |
| Large unsigned enum constant | Supported |        |
| Call with default arguments  | Supported |        |
| Macro-shadowed symbol        | Supported |        |

### MSVC inheritance model

`MSInheritanceAttr` does not corrupt record hover

clangd tracks this as clangd#1643 and clangd#2212; under an MSVC target
the implicit inheritance attribute does not leak into the record or
method card.

```cpp
namespace ms {

struct Widget {
    int value;
    void update();
};

int Widget::* member = &Widget::value;

}
```

### Most-vexing-parse

Object init and function declaration hover distinctly

clangd tracks this as clangd#2225; clice reads the direct-init as a
variable and the vexing form as a function declaration.

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

### Large unsigned enum constant

Hovering a `0xFFFF...ULL` enumerator does not crash

clangd crashes on this (clangd#2381); clice renders the full unsigned
value without overflow.

```cpp
namespace big_enum {

enum class Flags : unsigned long long {
    Max = 0xFFFFFFFFFFFFFFFFULL,
};

}
```

### Call with default arguments

Hovering a call that omits defaults does not crash

clangd crashes on this (clangd#551); clice renders the callee signature
with its default arguments.

```cpp
namespace defaults {

int compute(int a, int b = 10, int c = 20);

int result = compute(1);

}
```

### Macro-shadowed symbol

A function-like macro over a same-named function

clangd tracks this as clangd#2490; at the call site the function-like
macro is active, and clice's card shows that macro and its expansion.

```cpp
namespace shadow {

int lookup(int key) {
    return key;
}

}

#define lookup(key) ((key) + 100)

int value = lookup(5);
```

<!-- END GENERATED ITEMS -->
