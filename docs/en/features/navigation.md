# Code Navigation

## Go to Definition

<!-- BEGIN GENERATED ITEMS: go_to_definition -->

| Capability                                                             | Status      | Issues                                                      |
| ---------------------------------------------------------------------- | ----------- | ----------------------------------------------------------- |
| Cross-TU go-to-definition                                              | Supported   |                                                             |
| Definition and declaration alternate at the cursor site                | Supported   |                                                             |
| Declaration-only symbols navigate to their declaration                 | Supported   |                                                             |
| Go-to-definition on `#include` directives                              | Supported   |                                                             |
| Local variables and parameters navigate to their declaration           | Supported   |                                                             |
| Navigate through macro wrappers to the underlying declaration          | Supported   |                                                             |
| Names conjured by a macro body or token paste anchor at the invocation | Supported   |                                                             |
| Tokens inside a `#define` body carry no navigation of their own        | Supported   |                                                             |
| Error recovery                                                         | Unsupported |                                                             |
| Dependent member navigation in uninstantiated templates                | Supported   |                                                             |
| Template specialization navigates to the primary template              | Unsupported | [clangd#212](https://github.com/clangd/clangd/issues/212)   |
| `auto` keyword navigates to the deduced type                           | Unsupported | [clangd#2055](https://github.com/clangd/clangd/issues/2055) |

### Cross-TU go-to-definition

A use in one translation unit resolves to the definition supplied by
a sibling source — the answer spans the project, not the current
file alone.

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

### Definition and declaration alternate at the cursor site

On a use, go-to-definition reaches the definition. Invoked on the
definition it steps to the declaration, and on the declaration it
steps to the definition — the two sites alternate. A symbol defined
inline, with no separate declaration, keeps its definition as the
answer.

```cpp
int scale(int value);

int scale(int value) {
    return value * 2;
}

int apply(int value) {
    return scale(value);
}
```

### Declaration-only symbols navigate to their declaration

Symbols that carry only a declaration — pure virtuals, `extern`
variables, in-class static constants — resolve to that declaration
instead of returning nothing.

```cpp
extern int threshold;

int probe(int value);

struct Screen {
    static const int margin = 4;
    virtual void refresh() = 0;
};

int watch(Screen& screen, int value) {
    screen.refresh();
    return probe(value) + threshold + Screen::margin;
}
```

### Go-to-definition on `#include` directives

Invoked on an `#include` line, go-to-definition opens the included
file. This works for the leading includes compiled into the preamble
(the PCH) as well as ordinary ones later in the file.

`main.cpp`:

```cpp
#include "panel.h"

int build() {
    return dimension();
}

#include "extra.h"

int total() {
    return build() + spacing();
}
```

`extra.h`:

```cpp
inline int spacing() {
    return 2;
}
```

`panel.h`:

```cpp
#pragma once

int dimension();
```

### Local variables and parameters navigate to their declaration

Go-to-definition on a local variable or parameter jumps to its
declaration inside the function body.

```cpp
int accumulate(int base) {
    int total = base;
    total = total + base;
    return total;
}
```

### Navigate through macro wrappers to the underlying declaration

A name spelled in a macro argument anchors at its spelling, so
definition and declaration alternate there exactly as at a plain
site, and a later use resolves through the wrapper to the function it
declares.

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

### Names conjured by a macro body or token paste anchor at the invocation

A name assembled by token paste has no spelling of its own in the
source, so it anchors at the macro invocation that creates it: the
invocation is its definition site, and a plain use of the name jumps
back to that invocation.

```cpp
#define MAKE_FLAG(name) bool flag_##name = false

MAKE_FLAG(verbose);

bool read_flag() {
    return flag_verbose;
}
```

### Tokens inside a `#define` body carry no navigation of their own

A token written inside a macro body has no meaning until an expansion
assigns one, so navigation on it yields nothing, while the invocation
token always resolves to the macro being expanded.

```cpp
#define DEFINE_COUNTER int counter = 0

DEFINE_COUNTER;
```

### Error recovery

Navigate to a variable whose type is unresolved

When a variable's type name fails to resolve, go-to-definition on a
later use of the variable currently returns nothing, even though the
variable's own declaration is still recorded.

```cpp
Unresolved handle;  // 'Unresolved' does not name a type

void read() {
    (void) handle;  // go-to-def on handle → the declaration above
}
```

### Dependent member navigation in uninstantiated templates

Inside a template that is never instantiated, a member accessed on an
object of a dependent type resolves to the member declared on the
corresponding class template.

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

### Template specialization navigates to the primary template

Go-to-definition on the name of an explicit specialization resolves to
the specialization itself; stepping from it to the primary template it
specializes is not offered.

```cpp
template <typename T>
struct Formatter {}; // primary template

template <>
struct Formatter<int> {}; // go-to-def on Formatter → primary template
```

### `auto` keyword navigates to the deduced type

Go-to-definition on the `auto` keyword should reach the type it was
deduced to; today it returns nothing.

```cpp
struct Widget {};

Widget make_widget();

void use() {
    auto widget = make_widget(); // go-to-def on auto → Widget
}
```

<!-- END GENERATED ITEMS -->

### Implicit Code Navigation

Navigate to definitions of implicitly invoked code. In C++ many constructs generate hidden calls to constructors, operators, conversions, etc. Navigating from the syntactic construct (a brace, a keyword, an operator token) to the actual function being called is essential for understanding what code is really executing.

Implicit navigation requires an unambiguous source token — patterns where the token already has a well-defined go-to-def target (e.g., a variable name always goes to its declaration) cannot be repurposed for implicit call navigation.

<!-- BEGIN GENERATED ITEMS: implicit_code_navigation -->

| Capability                                          | Status      | Issues                                                      |
| --------------------------------------------------- | ----------- | ----------------------------------------------------------- |
| `override` / `final`                                | Unsupported |                                                             |
| `break` / `continue`                                | Unsupported | [clangd#1921](https://github.com/clangd/clangd/issues/1921) |
| Constructor calls                                   | Supported   |                                                             |
| Copy/move construction and assignment               | Partial     |                                                             |
| CTAD                                                | Supported   |                                                             |
| Aggregate initialization                            | Supported   |                                                             |
| `delete` expression                                 | Unsupported |                                                             |
| `new` expression                                    | Partial     |                                                             |
| Member initializer list                             | Partial     |                                                             |
| Delegating constructors                             | Partial     |                                                             |
| Inherited constructors                              | Partial     |                                                             |
| Return value implicit construction                  | Supported   |                                                             |
| Lambda init-capture                                 | Unsupported |                                                             |
| Overloaded operators                                | Supported   |                                                             |
| C++20 rewritten operators                           | Supported   |                                                             |
| User-defined literals                               | Unsupported |                                                             |
| Implicit conversion operators                       | Unsupported | [clangd#1931](https://github.com/clangd/clangd/issues/1931) |
| Casts invoking a constructor or conversion operator | Partial     |                                                             |
| Range-based for                                     | Unsupported |                                                             |
| Structured bindings                                 | Unsupported |                                                             |
| `co_await` / `co_yield` / `co_return`               | Partial     |                                                             |

### `override` / `final`

Navigate to the overridden base method

Go-to-definition on the `override` or `final` specifier should reach the
base class virtual method it overrides; today it returns nothing.

```cpp
struct Base {
    virtual void draw();
    virtual void paint();
};

struct Derived : Base {
    void draw() override;  // go-to-def on override → Base::draw
    void paint() final;    // go-to-def on final → Base::paint
};
```

### `break` / `continue`

Navigate to the enclosing loop or switch head

Go-to-definition on `break` or `continue` should reach the head of the
loop or switch it controls; today it returns nothing.

```cpp
void loop() {
    for (int i = 0; i < 10; i += 1) {
        if (i == 5) break;  // go-to-def on break → the for loop
        continue;           // go-to-def on continue → the for loop
    }
}
```

### Constructor calls

From parentheses or braces to the selected constructor

Go-to-definition on the opening parenthesis or brace of a constructor
call reaches the constructor overload resolution selected, for both the
`T(args)` and `T{args}` forms.

```cpp
struct Widget {
    Widget(int w, int h);
};

void build() {
    Widget a(800, 600);
    Widget b{800, 600};
}
```

### Copy/move construction and assignment

To the constructor or assignment operator

Go-to-definition on the `=` of an assignment reaches the assignment
operator. The `=` that introduces a copy- or move-initialization
(`T b = a;`) is initialization syntax rather than an operator call and is
not yet resolved.

```cpp
struct Widget {
    Widget(int v);
    Widget(const Widget& other);
    Widget(Widget&& other);
    Widget& operator=(const Widget& other);
};

void copies(Widget a) {
    Widget b = a;
    Widget c = static_cast<Widget&&>(a);
    b = c;
}
```

### CTAD

Navigate to the selected constructor

When class template argument deduction picks a specialization, go-to-
definition on the constructor call reaches the constructor that was
selected, not merely the class template.

```cpp
template <typename T>
struct Box {
    Box(T input) : value(input) {}
    T value;
};

template <typename T>
Box(T) -> Box<T>;

void use() {
    Box b(7);
}
```

### Aggregate initialization

Navigate to the struct definition

An aggregate has no constructor, so go-to-definition on its initializer
brace reaches the aggregate's definition.

```cpp
struct Point {
    int x;
    int y;
};

void use() {
    auto p = Point{1, 2};
}
```

### `delete` expression

Navigate to the destructor

Go-to-definition on `delete` should reach the destructor it runs; today
it returns nothing.

```cpp
struct Widget {
    ~Widget();
};

void dispose(Widget* widget) {
    delete widget;  // go-to-def on delete → Widget::~Widget
}
```

### `new` expression

Navigate to the constructor and overloaded `operator new`

Go-to-definition on `new` reaches the class's overloaded `operator new`.
The constructor invoked by the same expression is not part of the reply.

```cpp
struct Pool {
    Pool();
    static void* operator new(decltype(sizeof(0)) size);
};

void make() {
    Pool* p = new Pool();
}
```

### Member initializer list

Navigate to base and member constructors

The base and member constructors run by an initializer list are reached
from the opening parenthesis of each initializer. The initializer name
itself resolves to the base type or the member, so navigation to the
constructor goes through the parenthesis.

```cpp
struct Base {
    Base(int x);
};

struct Logger {
    Logger(int level);
};

struct App : Base {
    Logger logger;
    App() : Base(42), logger(1) {}
};
```

### Delegating constructors

Navigate to the target constructor

A delegating constructor's target is reached from the opening parenthesis
of the delegated call. The constructor name itself resolves to the class
type, so navigation to the target constructor goes through the
parenthesis.

```cpp
struct Widget {
    Widget(int w, int h);
    Widget() : Widget(0, 0) {}
};
```

### Inherited constructors

Navigate to the base constructors brought in by `using`

Go-to-definition on an inherited-constructor declaration
(`using Base::Base;`) reaches a base constructor. When the base declares
several constructors the reply resolves to one of them rather than
listing the whole set.

```cpp
struct Base {
    Base(int x);
    Base(int x, int y);
};

struct Derived : Base {
    using Base::Base;
};
```

### Return value implicit construction

Navigate to the constructor

A braced `return {args}` implicitly constructs the function's return
type; go-to-definition on the brace reaches the selected constructor.

```cpp
struct Widget {
    Widget(int w, int h);
};

Widget create() {
    return {800, 600};
}
```

### Lambda init-capture

Navigate to the constructor

Go-to-definition on the `=` of a lambda init-capture should reach the
constructor that builds the captured value; today it returns nothing.

```cpp
struct Widget {
    Widget(int v);
    Widget(Widget&& other);
};

void use(Widget w) {
    // go-to-def on = → Widget(Widget&&)
    auto f = [x = static_cast<Widget&&>(w)] {};
}
```

### Overloaded operators

From the operator token to its definition

Go-to-definition on an overloaded operator token reaches the operator's
definition. The binary, subscript, call and arrow operators (`+`, `[]`,
`()`, `->`) are all resolved.

```cpp
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
    Vec c = a + b;
    int e = a[0];
    int f = a(1, 2);
    a->value;
}
```

### C++20 rewritten operators

Navigate to the operator the rewrite uses

For a comparison synthesized by the C++20 rewrite rules, go-to-definition
on the written operator reaches the operator that actually implements it:
`!=` reaches `operator==`, and `>` reaches `operator<=>`.

```cpp
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
    bool ne = a != b;
    bool gt = a > b;
}
```

### User-defined literals

Navigate to the literal operator

Go-to-definition on a user-defined-literal suffix should reach its
`operator""`; today it returns nothing.

```cpp
struct Duration {
    unsigned long long ticks;
};

Duration operator""_ms(unsigned long long value);

void use() {
    Duration d = 500_ms;  // go-to-def on _ms → operator""_ms
}
```

### Implicit conversion operators

From a conversion context to the operator

Go-to-definition from a context that runs a user-defined conversion (a
condition, `!`, an explicit `bool(...)`) should reach the conversion
operator; today it returns nothing.

```cpp
struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if (g) {}      // go-to-def on ( → Guard::operator bool
    bool ok = !g;  // go-to-def on ! → Guard::operator bool
}
```

### Casts invoking a constructor or conversion operator

A `static_cast` that constructs its target reaches the selected
constructor. A `static_cast` that runs a user-defined conversion operator
does not yet reach the operator.

```cpp
struct Meters {
    explicit operator double() const;
};

struct Foo {
    explicit Foo(int value);
};

void use(Meters m) {
    double d = static_cast<double>(m);
    Foo f = static_cast<Foo>(42);
}
```

### Range-based for

Navigate to `begin()` / `end()`

Go-to-definition on the `:` of a range-based for should reach the
`begin()` / `end()` chosen for the range; today it returns nothing.

```cpp
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

### Structured bindings

Navigate to the underlying accessors or fields

Go-to-definition on a structured binding name resolves to the binding
itself rather than the underlying field or accessor it names.

```cpp
struct Pair {
    int first;
    int second;
};

void use(Pair p) {
    // go-to-def on a → Pair::first, on b → Pair::second
    auto [a, b] = p;
}
```

### `co_await` / `co_yield` / `co_return`

Navigate to the awaiter or promise method

Go-to-definition on `co_yield` reaches the promise's `yield_value`. The
`co_await` and `co_return` keywords do not yet reach the awaiter's or
promise's methods.

```cpp
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
    co_await Awaiter{};
    co_yield 1;
    co_return 2;
}
```

<!-- END GENERATED ITEMS -->

## Go to Declaration

Navigate from a symbol usage or definition to its declaration. In C++, many entities have separate declarations and definitions.

clice returns the declaration locations plus the definition — symbols defined inline have no separate declaration — minus the site the cursor already stands on, so declaration and definition sites alternate just like go-to-definition.

<!-- BEGIN GENERATED ITEMS: go_to_declaration -->

| Capability                                                        | Status    | Issues |
| ----------------------------------------------------------------- | --------- | ------ |
| Cross-TU go-to-declaration                                        | Supported |        |
| Functions                                                         | Supported |        |
| Forward declarations of classes and structs                       | Supported |        |
| Static data member                                                | Supported |        |
| `extern` variable                                                 | Supported |        |
| Multiple declarations                                             | Supported |        |
| Declaration and definition with cosmetically different signatures | Supported |        |

### Cross-TU go-to-declaration

Go-to-declaration on a use resolves sites in other files: the
prototype lives in a shared header and the out-of-line definition in a
sibling source, and both are offered from a use in another file.

`main.cpp`:

```cpp
#include "shared.h"

int run(int value) {
    return scale(value);
}
```

`lib.cpp`:

```cpp
#include "shared.h"

int scale(int value) {
    return value * 2;
}
```

`shared.h`:

```cpp
#pragma once

int scale(int value);
```

### Functions

From a use or out-of-line definition to the prototype

Go-to-declaration reaches a function's prototype both from a call site
and from the out-of-line definition — the two non-cursor sites the
prototype alternates with.

```cpp
struct Widget {
    void draw();
};

void Widget::draw() {}

void render(Widget& widget) {
    widget.draw();
}
```

### Forward declarations of classes and structs

A class with a forward declaration and a later definition offers both
from a use — the forward declaration stays part of the declaration set
rather than being dropped in favour of the definition.

```cpp
struct Widget;

struct Widget {
    int value;
};

class Panel;

class Panel {
    int width;
};

int probe(Widget& widget, Panel& panel) {
    return widget.value;
}
```

### Static data member

To the in-class declaration

A static data member is declared inside the class and defined out of
line; go-to-declaration on a use offers the in-class declaration
alongside the definition.

```cpp
struct Config {
    static int timeout;
};

int Config::timeout = 30;

int read_config() {
    return Config::timeout;
}
```

### `extern` variable

To the declaration

A use of an `extern` variable offers the `extern` declaration and
the defining declaration together, so the header-side declaration is
always reachable from a use.

```cpp
extern int log_level;

int log_level = 0;

int read_level() {
    return log_level;
}
```

### Multiple declarations

Every declaration site

When an entity is declared in several places, go-to-declaration on a
use lists every declaration site, not only the nearest one.

```cpp
int clamp(int value);
int clamp(int value);

int clamp(int value) {
    return value < 0 ? 0 : value;
}

int hold(int value) {
    return clamp(value);
}
```

### Declaration and definition with cosmetically different signatures

Parameter names, and a top-level `const` on a parameter, are not part
of a function's type: the declaration and the definition below spell the
same function differently, yet go-to-declaration still connects a use to
the prototype.

```cpp
int render(int width, const int height);

int render(int w, int h) {
    return w * h;
}

int use_render() {
    return render(800, 600);
}
```

<!-- END GENERATED ITEMS -->

## Go to Implementation

<!-- BEGIN GENERATED ITEMS: go_to_implementation -->

| Capability                    | Status      | Issues                                                    |
| ----------------------------- | ----------- | --------------------------------------------------------- |
| Override chain                | Supported   |                                                           |
| Sibling overrides             | Supported   |                                                           |
| Non-virtual function          | Unsupported | [clangd#854](https://github.com/clangd/clangd/issues/854) |
| Base class                    | Supported   |                                                           |
| Template duck-type navigation | Unsupported |                                                           |

### Override chain

Each level of a chain to its own overriders

Along a three-level override chain, go-to-implementation from each method
reaches the override one level down — base to middle, middle to leaf.

```cpp
struct Base {
    virtual void run() = 0;
};

struct Middle : Base {
    void run() override {}
};

struct Leaf : Middle {
    void run() override {}
};
```

### Sibling overrides

Every sibling override

Go-to-implementation on a virtual method lists every override across
the sibling derived classes.

```cpp
struct Shape {
    virtual int area() = 0;
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
```

### Non-virtual function

Declaration to out-of-line definition

Go-to-implementation on a non-virtual function declaration should reach
its out-of-line definition, behaving as a superset of go-to-definition;
today it returns nothing.

```cpp
struct Widget {
    void draw();  // go-to-impl on draw → out-of-line definition below
};

void Widget::draw() {}
```

### Base class

Every derived class

Go-to-implementation on a base class name lists the classes that derive
from it.

```cpp
struct Base {};

struct Circle : Base {};

struct Square : Base {};
```

### Template duck-type navigation

From a dependent member call, go-to-implementation should list the
concrete methods of every known instantiation; the same applies to a
generic lambda's dependent calls. Today it returns nothing.

```cpp
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

<!-- END GENERATED ITEMS -->

## Go to Type Definition

Navigate to the type definition of a symbol. Applicable to variables, parameters, fields, and any other named entity that has a type. When the type is a type alias or a pointer-like wrapper, navigation should unwrap to the underlying/pointee type.

<!-- BEGIN GENERATED ITEMS: go_to_type_definition -->

| Capability                        | Status      | Issues                                                      |
| --------------------------------- | ----------- | ----------------------------------------------------------- |
| Variables and parameters          | Supported   |                                                             |
| Class and struct fields           | Supported   |                                                             |
| `auto`-deduced variables          | Unsupported |                                                             |
| Smart pointer to the pointee type | Partial     | [clangd#1026](https://github.com/clangd/clangd/issues/1026) |
| Type aliases                      | Partial     |                                                             |
| Structured binding variables      | Supported   |                                                             |

### Variables and parameters

Go-to-type-definition on a local variable or a parameter reaches the
definition of its type.

```cpp
struct Widget {};

Widget make_widget();

int probe(Widget param) {
    Widget local = make_widget();
    return 0;
}
```

### Class and struct fields

Go-to-type-definition on a field access reaches the definition of the
field's type.

```cpp
struct Logger {};

class Store {};

struct App {
    Logger logger;
    Store store;
};

int use(App& app) {
    app.logger;
    app.store;
    return 0;
}
```

### `auto`-deduced variables

Go-to-type-definition on an `auto`-deduced variable should reach the
deduced type's definition; today the variable carries no type relation,
so it returns nothing.

```cpp
struct Widget {};

Widget make_widget();

void probe() {
    auto widget = make_widget();  // go-to-type-def on widget → Widget
}
```

### Smart pointer to the pointee type

Go-to-type-definition on a smart-pointer variable reaches the wrapper
type itself; unwrapping to the pointee type is not offered.

```cpp
template <typename T>
struct Ptr {
    T* operator->();
    T& operator*();
    T* raw;
};

struct Widget {};

int use(Ptr<Widget> ptr) {
    return 0;
}
```

### Type aliases

Go-to-type-definition on a variable of an aliased type reaches the
`using` or `typedef` declaration; it does not yet unwrap the alias to
the underlying type's definition.

```cpp
struct Impl {};

using Handle = Impl;

typedef Impl LegacyHandle;

int use(Handle handle, LegacyHandle legacy) {
    return 0;
}
```

### Structured binding variables

Go-to-type-definition on a structured binding reaches the definition of
the bound member's type.

```cpp
struct Widget {};

struct Pair {
    Widget first;
    int second;
};

Pair make_pair();

int use() {
    auto [widget, count] = make_pair();
    return 0;
}
```

<!-- END GENERATED ITEMS -->

## Find References

<!-- BEGIN GENERATED ITEMS: find_references -->

| Capability                                                          | Status      | Issues                                                                                                                 |
| ------------------------------------------------------------------- | ----------- | ---------------------------------------------------------------------------------------------------------------------- |
| Cross-TU find references                                            | Supported   |                                                                                                                        |
| Declaration and definition sites appear among references            | Supported   |                                                                                                                        |
| Implicit references from range-based for loops                      | Unsupported | [clangd#1081](https://github.com/clangd/clangd/issues/1081)                                                            |
| Implicit constructor and destructor calls                           | Unsupported |                                                                                                                        |
| References through forwarding functions                             | Unsupported | [clangd#716](https://github.com/clangd/clangd/issues/716), [clangd#1872](https://github.com/clangd/clangd/issues/1872) |
| References in dependent and template contexts                       | Unsupported | [clangd#258](https://github.com/clangd/clangd/issues/258), [clangd#675](https://github.com/clangd/clangd/issues/675)   |
| Read/write classification of references                             | Unsupported | [clangd#2139](https://github.com/clangd/clangd/issues/2139)                                                            |
| Enclosing function shown with each reference                        | Unsupported | [clangd#177](https://github.com/clangd/clangd/issues/177)                                                              |
| Macro references across expansions, `#ifdef`/`#ifndef` and `#undef` | Supported   |                                                                                                                        |
| Macro references spelled inside other macro definitions             | Unsupported | [clangd#346](https://github.com/clangd/clangd/issues/346)                                                              |
| Label and goto references                                           | Supported   |                                                                                                                        |

### Cross-TU find references

Find references gathers uses from other files too: a function
defined in one source and called from a sibling reports both call
sites together with the declaration in the shared header, not only the
uses in the current file.

`main.cpp`:

```cpp
#include "shared.h"

int run(int value) {
    return compute(value);
}
```

`lib.cpp`:

```cpp
#include "shared.h"

int compute(int value) {
    return value * 2;
}

int again(int value) {
    return compute(value) + 1;
}
```

`shared.h`:

```cpp
#pragma once

int compute(int value);
```

### Declaration and definition sites appear among references

A reference query returns the declaration and the out-of-line
definition together with every use, so the whole surface of a symbol
is reachable from any one of its sites.

```cpp
int scale(int value);

int scale(int value) {
    return value * 2;
}

int use() {
    return scale(3);
}
```

### Implicit references from range-based for loops

Find references on `begin` reports only its own declaration; the
range-based for loop that implicitly calls it is not included among the
references.

```cpp
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

### Implicit constructor and destructor calls

Find references on a constructor reports only its explicit sites; an
object definition that implicitly invokes the constructor or its
destructor is not included.

```cpp
struct Blob {
    Blob();  // find-refs here omits the `Blob b;` definition below
    ~Blob();
};

void use() {
    Blob b;
}
```

### References through forwarding functions

Find references on a constructor does not include call sites that reach
it indirectly through a perfect-forwarding factory.

```cpp
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

### References in dependent and template contexts

Find references on a member does not include dependent call sites in a
template, even when the template is instantiated with the member's
class.

```cpp
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

### Read/write classification of references

The reference reply carries only locations, so a reader cannot tell a
write from a read; annotating each result with its access kind is not
offered.

```cpp
int use() {
    int x = 0;      // write
    int y = x + 1;  // read
    x = y;          // write
    return x;
}
```

### Enclosing function shown with each reference

Each reference is reported as a bare location; the name of the function
that encloses it is not attached, so results carry no context beyond
the file and line.

```cpp
int shared_value = 0;

int reader() {
    return shared_value;
}

int writer() {
    shared_value = 1;
    return shared_value;
}
```

### Macro references across expansions, `#ifdef`/`#ifndef` and `#undef`

A macro's references span its expansions, the `#ifdef` / `#ifndef`
conditionals that test it and the `#undef` that cancels it. Each
`#define` of a name is its own symbol, so a redefinition after `#undef`
collects only its own uses.

```cpp
#define FEATURE 1

int on = FEATURE;

#ifdef FEATURE
int guarded = 1;
#endif

#ifndef FEATURE
int missing = 0;
#endif

#undef FEATURE

#define FEATURE 2

int again = FEATURE;
```

### Macro references spelled inside other macro definitions

Find references on a macro does not include the mentions of it written
inside the bodies of other macro definitions.

```cpp
#define WIDTH 100  // find-refs here omits the WIDTH tokens in AREA below

#define AREA (WIDTH * WIDTH)

int total = AREA;
```

### Label and goto references

Find references on a label lists the label itself together with every
`goto` that jumps to it.

```cpp
int loop(int failed) {
    retry:
    if (failed) {
        goto retry;
    }
    return 0;
}
```

<!-- END GENERATED ITEMS -->

## Call Hierarchy

<!-- BEGIN GENERATED ITEMS: call_hierarchy -->

| Capability                                      | Status      | Issues                                                      |
| ----------------------------------------------- | ----------- | ----------------------------------------------------------- |
| Prepare call hierarchy on functions and methods | Supported   |                                                             |
| Incoming calls                                  | Supported   |                                                             |
| Outgoing calls                                  | Supported   |                                                             |
| Function signature in the item detail           | Unsupported |                                                             |
| Qualified name for member functions             | Partial     |                                                             |
| Follow virtual dispatch                         | Unsupported |                                                             |
| Non-function targets                            | Unsupported | [clangd#1308](https://github.com/clangd/clangd/issues/1308) |
| Calls inside lambdas                            | Supported   |                                                             |
| Constructor calls through forwarding functions  | Unsupported | [clangd#2242](https://github.com/clangd/clangd/issues/2242) |

### Prepare call hierarchy on functions and methods

Preparing a call hierarchy works on a free function and on a member
method alike, anchoring an item at the entity under the cursor.

```cpp
struct Service {
    void start();
};

void Service::start() {}

void launch(Service& s) {
    s.start();
}
```

### Incoming calls

Incoming calls list every caller of a function, and a caller that
invokes it more than once contributes each call site.

```cpp
int helper(int v) {
    return v;
}

int alpha() {
    return helper(1);
}

int beta() {
    return helper(2) + helper(3);
}
```

### Outgoing calls

Outgoing calls list every function a body invokes, one entry per
callee.

```cpp
int one() {
    return 1;
}

int two() {
    return 2;
}

int three() {
    return 3;
}

int dispatch() {
    return one() + two() + three();
}
```

### Function signature in the item detail

A call hierarchy item carries only its name; the function signature is
not attached in a detail field, so overloads are indistinguishable in
the hierarchy.

```cpp
int compute(int a, int b) {  // no signature attached to this item
    return a + b;
}

int caller() {
    return compute(1, 2);
}
```

### Qualified name for member functions

A member function's call hierarchy item is produced, but its name field
carries only the bare method name (`draw`), not the qualified
`Circle::draw` that would tell it apart from a free function.

```cpp
struct Circle {
    void draw();
};

void Circle::draw() {}
```

### Follow virtual dispatch

Incoming calls of a base virtual method do not include calls made
through derived overrides; a call to an override is attributed only to
that override, never to the base it overrides.

```cpp
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

### Non-function targets

Variables and enum constants

Preparing a call hierarchy on a variable or an enum constant returns
nothing; the request is offered only for functions and methods.

```cpp
int counter = 0;  // prepare call hierarchy here → nothing

enum Mode {
    Fast,  // prepare call hierarchy here → nothing
    Slow,
};
```

### Calls inside lambdas

A call written in a lambda body appears in the incoming calls of the
function it invokes, attributed to the function that encloses the
lambda.

```cpp
void foo() {}

void use() {
    auto task = [] {
        foo();
    };
    task();
}
```

### Constructor calls through forwarding functions

Incoming calls of a constructor do not include the call sites that
reach it through a perfect-forwarding factory.

```cpp
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

<!-- END GENERATED ITEMS -->

## Type Hierarchy

<!-- BEGIN GENERATED ITEMS: type_hierarchy -->

| Capability                                              | Status    | Issues                                                  |
| ------------------------------------------------------- | --------- | ------------------------------------------------------- |
| Prepare type hierarchy on class, struct, enum and union | Supported |                                                         |
| Supertypes                                              | Supported |                                                         |
| Subtypes                                                | Supported |                                                         |
| Template inheritance                                    | Supported |                                                         |
| Template arguments in type hierarchy items              | Partial   | [clangd#31](https://github.com/clangd/clangd/issues/31) |

### Prepare type hierarchy on class, struct, enum and union

Preparing a type hierarchy anchors an item on any user-defined type
tag — class, struct, enum and union alike.

```cpp
class Handle {};

struct Point {};

enum class Mode {};

union Storage {
    int i;
    float f;
};
```

### Supertypes

Supertypes list every direct base of a class, including each base of a
multiple-inheritance derived type.

```cpp
struct Alpha {};

struct Beta {};

struct Gamma : Alpha, Beta {};
```

### Subtypes

Subtypes list every class that derives from a base, across sibling
derived types.

```cpp
struct Shape {};

struct Circle : Shape {};

struct Square : Shape {};

struct Triangle : Shape {};
```

### Template inheritance

Subtypes of a base include classes that derive from it through a class
template, such as a CRTP wrapper.

```cpp
struct Base {};

template <typename T>
struct CRTP : Base {};

struct Widget : CRTP<Widget> {};
```

### Template arguments in type hierarchy items

A subtype produced by a class template specialization is listed, but
its item name carries only the bare template name (`Derived`), without
the template arguments that would distinguish `Derived<Foo>`.

```cpp
struct Foo {};

struct Base {};

template <typename T>
struct Derived : Base {};

Derived<Foo> instance;
```

<!-- END GENERATED ITEMS -->

## Workspace Symbol

Search the whole project for a symbol by name (`workspace/symbol`).

<!-- BEGIN GENERATED ITEMS: workspace_symbol -->

| Capability                                        | Status      | Issues                                                      |
| ------------------------------------------------- | ----------- | ----------------------------------------------------------- |
| Basic workspace-wide symbol search                | Supported   |                                                             |
| Search spans the whole project                    | Supported   |                                                             |
| Overload disambiguation                           | Partial     | [clangd#1344](https://github.com/clangd/clangd/issues/1344) |
| Fuzzy matching                                    | Unsupported | [clangd#914](https://github.com/clangd/clangd/issues/914)   |
| Partially qualified name search                   | Unsupported | [clangd#550](https://github.com/clangd/clangd/issues/550)   |
| Enumerator lookup under the enum's scope          | Unsupported | [clangd#931](https://github.com/clangd/clangd/issues/931)   |
| Underlying declarations ranked above type aliases | Unsupported | [clangd#2253](https://github.com/clangd/clangd/issues/2253) |
| Search by mangled (linker) name                   | Unsupported |                                                             |

### Basic workspace-wide symbol search

case-insensitive substring matching

A query matches any symbol whose name contains it, ignoring case:
functions, types, enumerators and macros all participate, and a query
with no match returns an empty list rather than an error.

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

### Search spans the whole project

Hits from files other than the queried one

The query returns symbols from project files that are not even open
in the editor: `other.h` stays closed here, so its hit is served by
the background index.

`main.cpp`:

```cpp
// query: helper_elsewhere

int local_anchor = 0;
```

`other.h`:

```cpp
void helper_elsewhere() {}
```

### Overload disambiguation

Parameter types shown in results

Querying an overloaded name finds every overload, but each entry
carries only the bare name — nothing tells the two `process` results
apart short of opening both locations.

```cpp
// query: process

void process(int value) {}

void process(bool flag, int level) {}
```

### Fuzzy matching

word-boundary-aware scoring for camelCase and snake_case

Matching is a case-insensitive substring test: `LinLis` does not find
`LinkedList`, and `pcfg` does not find `parse_config`. Word-boundary
initials should match and score for every symbol kind, macros
included.

```cpp
// query: LinLis
// query: pcfg

struct LinkedList {};

void parse_config();
```

### Partially qualified name search

Symbols match by bare name only: `net::Socket` finds nothing even
though `deep::net::Socket` exists, and neither does any other
qualifier-prefixed form.

```cpp
// query: net::Socket

namespace deep {
namespace net {

struct Socket {};

}  // namespace net
}  // namespace deep
```

### Enumerator lookup under the enum's scope

`Color::Red` should find the enumerator — for scoped and unscoped
enums alike — but qualified queries match nothing; only the bare
`Red` does.

```cpp
// query: Color::Red

enum Color { Red, Green };
```

### Underlying declarations ranked above type aliases

When both `ConnectionImpl` and its alias `Connection` match a query,
the underlying declaration should rank first. Results carry no
ranking today.

```cpp
// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
```

### Search by mangled (linker) name

Pasting a linker symbol such as `_Z7processi` should resolve to the
function it mangles — useful when chasing linker errors and stack
traces.

```cpp
// query: _Z7processi

void process(int value);
```

<!-- END GENERATED ITEMS -->

## Module Navigation

<!-- BEGIN GENERATED ITEMS: module_navigation -->

| Capability                                                        | Status    | Issues                                                      |
| ----------------------------------------------------------------- | --------- | ----------------------------------------------------------- |
| `import module_name` navigates to the module interface unit       | Supported | [clangd#2310](https://github.com/clangd/clangd/issues/2310) |
| `import :partition` navigates to the partition unit               | Supported |                                                             |
| Navigate between interface and implementation units of one module | Partial   |                                                             |
| Dot-separated module name                                         | Partial   |                                                             |

### `import module_name` navigates to the module interface unit

Go-to-definition on the name in an `import` declaration opens the
module interface unit that exports it, and uses of an imported symbol
reach its definition in that unit.

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

### `import :partition` navigates to the partition unit

Go-to-definition on the partition name after the colon in a partition
import opens the partition unit that declares it.

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

### Navigate between interface and implementation units of one module

Go-to-definition on the module name in an implementation unit
(`module m;`) jumps to the interface unit that declares the module;
the reverse direction, from the interface name to the implementation,
is not offered.

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

### Dot-separated module name

Navigate each segment

Go-to-definition on the leading segment of a dot-separated module name
reaches the module's interface unit; the segments after a dot do not
resolve on their own yet.

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

<!-- END GENERATED ITEMS -->

## Document Highlight

Highlight all references to the symbol under cursor within the current file (`textDocument/documentHighlight`).

<!-- BEGIN GENERATED ITEMS: document_highlight -->

| Capability                                                                   | Status      | Issues                                                      |
| ---------------------------------------------------------------------------- | ----------- | ----------------------------------------------------------- |
| Highlight every reference to the symbol under the cursor in the current file | Unsupported |                                                             |
| Read/write classification for symbol highlights                              | Unsupported |                                                             |
| Control flow token highlighting                                              | Unsupported | [clangd#1921](https://github.com/clangd/clangd/issues/1921) |

### Highlight every reference to the symbol under the cursor in the current file

Placing the cursor on `total` should light up its declaration and
every use in the file; the request is not implemented.

```cpp
int total = 0;

void accumulate(int amount) {
    total = total + amount;
}
```

### Read/write classification for symbol highlights

Each highlight should carry its access kind, so editors can tint
writes differently from reads.

```cpp
void tally() {
    int count = 0;      // write
    int next = count;   // read
    count = next;       // write
}
```

### Control flow token highlighting

Highlighting `break` or `continue` should also light up the loop or
`switch` it belongs to — and `return` / `throw` the function exits
they mark.

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

<!-- END GENERATED ITEMS -->

## Switch Source/Header

<!-- BEGIN GENERATED ITEMS: switch_source_header -->

| Capability                                  | Status      | Issues |
| ------------------------------------------- | ----------- | ------ |
| Switch between a source file and its header | Unsupported |        |

### Switch between a source file and its header

From `widget.cpp` a single command should jump to `widget.h` and
back — the `textDocument/switchSourceHeader` request clangd clients
rely on is not implemented.

```cpp
// widget.h
class Widget {
    void draw();
};

// widget.cpp — #include "widget.h"
void Widget::draw() {}
```

<!-- END GENERATED ITEMS -->
