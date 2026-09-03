# Document Symbols

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`: a nested symbol tree with ranges, selection ranges and a `detail` field that disambiguates overloads and shows declared types.

## Symbol Hierarchy

<!-- BEGIN GENERATED ITEMS: symbol_hierarchy -->

| Capability                         | Status      | Issues                                                    |
| ---------------------------------- | ----------- | --------------------------------------------------------- |
| Nested symbol tree                 | Supported   |                                                           |
| Symbol ranges and selection ranges | Supported   |                                                           |
| Access specifier grouping          | Unsupported | [clangd#499](https://github.com/clangd/clangd/issues/499) |
| Anonymous and inline scopes        | Supported   |                                                           |
| UTF-16 position encoding           | Supported   |                                                           |

### Nested symbol tree

Symbols nest by their written scope; out-of-line definitions appear at their lexical position with qualified names

```cpp
namespace demo {

struct Point {
    int x;
    int y;

    int manhattan() const;
};

int Point::manhattan() const {
    return x + y;
}

enum class Axis { X, Y };

int origin_distance(const Point& p);

namespace inner {
constexpr int level = 2;
}

}  // namespace demo

// A reopened namespace gets its own outline node per written scope.
namespace demo {
int reopened();
}

namespace demo::nested {
int compact();
}
```

### Symbol ranges and selection ranges

The range spans the whole declaration; the selection range covers the full written name, including multi-token names like `~Widget`, `operator==` and `operator bool`

```cpp
namespace members {

struct Widget {
    Widget();
    explicit Widget(int size);
    ~Widget();

    Widget& operator=(const Widget& other);
    bool operator==(const Widget& other) const;
    operator bool() const;

    static int instances();

    int size;
    unsigned bits : 3;
    const char* name = "widget";
};

Widget::Widget(int size) : size(size), bits(0) {}

int Widget::instances() {
    return 0;
}

}  // namespace members
```

### Access specifier grouping

`public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation

```cpp
class Widget {
public:
    void draw();
    void resize();

private:
    int width;
    int height;
};
```

### Anonymous and inline scopes

Anonymous namespaces, unnamed structs and unions group their members under a placeholder name; inline namespace members stay under the inline namespace node

```cpp
namespace {

int hidden_counter = 0;

}  // namespace

namespace misc {

inline namespace v1 {

int versioned();

}  // namespace v1

struct Outer {
    struct {
        int anonymous_member;
    };

    union {
        int as_int;
        float as_float;
    };
};

}  // namespace misc
```

### UTF-16 position encoding

Columns after non-ASCII text count UTF-16 code units

```cpp
// π ≈ 3.14159, 中文注释
constexpr double 半径 = 2.0;
constexpr double π值 = 3.14159; double area();
```

<!-- END GENERATED ITEMS -->

## Symbol Kinds

<!-- BEGIN GENERATED ITEMS: symbol_kinds -->

| Capability                                    | Status    | Issues                                                            |
| --------------------------------------------- | --------- | ----------------------------------------------------------------- |
| Core symbol kinds                             | Supported |                                                                   |
| Template declarations                         | Supported |                                                                   |
| Template specializations and deduction guides | Supported |                                                                   |
| Type aliases                                  | Supported |                                                                   |
| Explicit instantiation directives             | Partial   | [llvm#191658](https://github.com/llvm/llvm-project/issues/191658) |
| Macro definitions                             | Supported | [clangd#1744](https://github.com/clangd/clangd/issues/1744)       |
| Macros in the preamble region                 | Partial   |                                                                   |

### Core symbol kinds

namespaces, classes, structs, unions, enums and their members, functions, variables, fields, structured bindings and lambdas all appear in the outline with a mapped LSP symbol kind

```cpp
namespace kinds {

union Value {
    int i;
    float f;
};

enum Flags { FlagA, FlagB };

enum class Mode : unsigned char { Fast, Safe };

struct Pair {
    struct Meta {
        int tag;
    };

    int first;
    int second;
    static int instances;
};

Pair make_pair();

auto [bound_first, bound_second] = make_pair();

auto lambda = [](int x) {
    return x * 2;
};

}  // namespace kinds
```

### Template declarations

class, function and variable templates carry a `template ` detail prefix; concepts and abbreviated function templates (`concept auto` parameters) appear as well

```cpp
namespace templates {

template <typename T>
struct Box {
    T value;

    void reset();
};

template <typename T>
void Box<T>::reset() {}

template <typename T>
T zero() {
    return T();
}

template <typename T>
constexpr T pi = T(3.14159);

template <typename T>
concept Small = sizeof(T) <= 4;

void takes_concept(Small auto x);

}  // namespace templates
```

### Template specializations and deduction guides

Explicit and partial specializations of class and variable templates appear with their template arguments in the name; members nest under their specialization; deduction guides render their deduced signature

```cpp
namespace spec {

template <typename T>
struct Box {
    T value;
};

template <>
struct Box<void> {};

template <typename T>
struct Box<T*> {
    T* pointee;
};

template <typename T>
T zero() {
    return T();
}

template <>
int zero<int>();

template <typename T>
constexpr T pi = T(3);

template <>
constexpr int pi<int> = 3;

template <typename T>
constexpr T* pi<T*> = nullptr;

template <typename T>
struct Deduced {
    Deduced(T raw);
};

template <typename T>
Deduced(T*) -> Deduced<T>;

// Forces the implicit instantiation Box<int>, which must not appear.
Box<int> instantiated;

// An explicit class instantiation gets a childless node; the instantiated
// members and the function instantiation (whose location clang records at
// the primary) produce no symbols.
template struct Box<char>;
template long zero<long>();

}  // namespace spec
```

### Type aliases

`typedef`, `using` aliases and alias templates appear in the outline with a `type alias` detail

```cpp
namespace aliases {

struct Widget {};

typedef Widget LegacyWidget;

using ModernWidget = Widget;

template <typename T>
struct Box {};

template <typename T>
using BoxOf = Box<T>;

struct Holder {
    using Inner = Widget;
};

}  // namespace aliases
```

### Explicit instantiation directives

The class forms appear as childless symbols; clang mislocates the function and variable forms at the pattern, so they are missing from the outline

```cpp
template <typename T>
struct Box {
    T value;
};

template struct Box<int>;
extern template struct Box<char>;

template <typename T>
void convert(T value) {}

template void convert<int>(int);

template <typename T>
T zero = T();

template int zero<int>;
```

### Macro definitions

object-like and function-like macro definitions in the outline, a parameter list as the function-like detail

```cpp
// The assertion holds the directives out of the preamble region, whose
// live record the server path does not yet see.
static_assert(true);

#define MAX_BUFFER_SIZE 4096
#define CHECK(cond, msg) ((cond) ? 0 : (msg))
#define TRACE(...) log(__VA_ARGS__)
#define SPLIT_\
LIMIT 7

struct Config {
#define CONFIG_VERSION 3
    int version = CONFIG_VERSION;
};
```

### Macros in the preamble region

Definitions in the leading directive run outline on the inspect path, while the server's preamble record does not surface them yet

```cpp
#define PREAMBLE_LIMIT 8
#define PREAMBLE_CHECK(cond) (!!(cond))

int after = PREAMBLE_LIMIT;
```

<!-- END GENERATED ITEMS -->

## Symbol Detail

<!-- BEGIN GENERATED ITEMS: symbol_detail -->

| Capability                 | Status      | Issues                                                                                                                                                                            |
| -------------------------- | ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Function signatures        | Supported   | [clangd#520](https://github.com/clangd/clangd/issues/520), [clangd#601](https://github.com/clangd/clangd/issues/601), [clangd#1232](https://github.com/clangd/clangd/issues/1232) |
| Variable and field types   | Supported   |                                                                                                                                                                                   |
| Default argument stripping | Supported   | [clangd#221](https://github.com/clangd/clangd/issues/221)                                                                                                                         |
| Base classes in detail     | Unsupported |                                                                                                                                                                                   |
| Multiline signature ranges | Supported   | [clangd#2221](https://github.com/clangd/clangd/issues/2221)                                                                                                                       |
| Scoped types               | Supported   |                                                                                                                                                                                   |

### Function signatures

Parameter and return types in the `detail` field disambiguate overloads; constructors drop the `void` return type

```cpp
namespace detail {

void process(int x);
void process(const char* s);

struct Task {
    Task();
    Task(int priority);

    int run(bool async) const;
};

}  // namespace detail
```

### Variable and field types

The declared type in the `detail` field; lambdas render as `(lambda)`

```cpp
namespace detail {

int timeout = 30;
const char* logger_name = "core";

struct Config {
    unsigned retries;
    double backoff;
};

auto on_error = [](int code) {
    return code != 0;
};

}  // namespace detail
```

### Default argument stripping

The signature is derived from the function type, so default parameter values never leak into the outline

```cpp
namespace detail {

void open_file(const char* path, int mode = 0644);

struct Server {
    void listen(int port = 8080, int backlog = 128);
};

}  // namespace detail
```

### Base classes in detail

Show `: Shape` on derived class declarations

```cpp
struct Shape {};

struct Circle : Shape {
    double radius;
};
```

### Multiline signature ranges

The symbol range starts at the beginning of the declaration and spans the full signature, so editor sticky scroll anchors correctly

```cpp
struct Config {};

void process_data(
    const Config& cfg,
    int flags
) {}
```

### Scoped types

A written class scope appears in the detail exactly once, for nested classes, template-ids, aliases and dependent names alike

```cpp
namespace scoped {

struct Outer {
    struct Inner {};
    template <typename T> struct Box {};
    using Alias = int;
};

struct User {
    Outer::Inner plain;
    Outer::Box<int> boxed;
    Outer::Alias aliased;
    const Outer::Inner frozen;
};

template <typename T>
struct Holder {
    typename T::type value;
    typename T::inner::type deep;
    typename T::template rebind<int> bound;
};

}  // namespace scoped
```

<!-- END GENERATED ITEMS -->

## Missing Symbols

<!-- BEGIN GENERATED ITEMS: missing_symbols -->

| Capability                        | Status      | Issues                                                      |
| --------------------------------- | ----------- | ----------------------------------------------------------- |
| Include directives                | Unsupported | [clangd#2226](https://github.com/clangd/clangd/issues/2226) |
| Local symbols                     | Supported   | [clangd#616](https://github.com/clangd/clangd/issues/616)   |
| Module declarations               | Unsupported |                                                             |
| `#pragma mark` navigation markers | Unsupported |                                                             |
| Friend function definitions       | Supported   |                                                             |

### Include directives

`#include` entries in the outline

```cpp
#include "config.h"

int uses_config();
```

### Local symbols

Variables and types declared inside function bodies nest under their function

```cpp
int compute() {
    int local_sum = 0;

    struct Accumulator {
        int total;
    };

    auto twice = [](int x) {
        return 2 * x;
    };

    struct Pair {
        int a;
        int b;
    };

    auto [first, second] = Pair{1, 2};

    return local_sum + twice(first) + second;
}
```

### Module declarations

`export module`, `module` and `import` declarations in the outline

```cpp
export module app.core;

import std;

export int core_entry();
```

### `#pragma mark` navigation markers

Editor section markers as outline entries

```cpp
#pragma mark - Lifecycle

void setup();

#pragma mark - Rendering

void draw();
```

### Friend function definitions

A friend function defined inline in a class appears under that class

```cpp
struct Owner {
    friend void inline_friend(Owner& o) {}

    friend bool operator==(const Owner& lhs, const Owner& rhs) {
        return &lhs == &rhs;
    }
};
```

<!-- END GENERATED ITEMS -->

## Symbol Tags

<!-- BEGIN GENERATED ITEMS: symbol_tags -->

| Capability                    | Status      | Issues                                                      |
| ----------------------------- | ----------- | ----------------------------------------------------------- |
| Deprecated tag                | Unsupported |                                                             |
| Access and storage indicators | Unsupported | [clangd#2123](https://github.com/clangd/clangd/issues/2123) |

### Deprecated tag

Mark `[[deprecated]]` symbols with the LSP `deprecated` symbol tag

```cpp
[[deprecated("use open_v2")]] void open_v1();

void open_v2();
```

### Access and storage indicators

Public / private / protected, static, virtual and abstract markers on outline entries

```cpp
class Base {
public:
    virtual void render() = 0;

protected:
    static int instances();

private:
    int id;
};
```

<!-- END GENERATED ITEMS -->

## Location Correctness

<!-- BEGIN GENERATED ITEMS: location_correctness -->

| Capability                       | Status    | Issues                                                      |
| -------------------------------- | --------- | ----------------------------------------------------------- |
| Symbols from macro expansions    | Supported | [clangd#475](https://github.com/clangd/clangd/issues/475)   |
| Names spelled in macro arguments | Supported | [clangd#1941](https://github.com/clangd/clangd/issues/1941) |

### Symbols from macro expansions

A symbol produced by a macro invocation is located at the invocation, not at the macro definition

```cpp
// The assertion holds the directives out of the preamble region, whose
// live record the server path does not yet see.
static_assert(true);

#define DEFINE_HANDLER(name) void name()

DEFINE_HANDLER(on_ready);
DEFINE_HANDLER(on_close);

#define DECLARE_CLASS(X) class X
DECLARE_CLASS(Generated) {
    int member;
};
```

### Names spelled in macro arguments

The selection range points at the name written in the macro argument; names spelled in the macro body fall back to the invocation site

```cpp
// The assertion holds the directives out of the preamble region, whose
// live record the server path does not yet see.
static_assert(true);

#define VAR(X) int X = 1;

VAR(from_argument)

#define COUNTER() int counter_from_body = 0;

COUNTER()
```

<!-- END GENERATED ITEMS -->
