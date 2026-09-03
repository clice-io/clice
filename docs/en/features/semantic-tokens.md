# Semantic Tokens

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice classifies every token of a document with its own token-kind vocabulary,
which is richer than the standard LSP token types and consistent across all
clice replies. Clients that prefer standard LSP kinds can map them through
configuration.

## Lexical Tokens

Kinds derived from the token stream itself, independent of the AST.

<!-- BEGIN GENERATED ITEMS: lexical_tokens -->

| Capability                            | Status      | Issues                                                      |
| ------------------------------------- | ----------- | ----------------------------------------------------------- |
| Comments                              | Supported   |                                                             |
| Literals                              | Supported   |                                                             |
| Keywords                              | Supported   |                                                             |
| Preprocessor directives               | Supported   |                                                             |
| Inactive regions                      | Supported   |                                                             |
| Header names                          | Supported   |                                                             |
| Inactive regions at the top of a file | Supported   |                                                             |
| Literal prefixes and suffixes         | Unsupported |                                                             |
| Escape sequences                      | Unsupported |                                                             |
| Declarator vs operator disambiguation | Unsupported | [clangd#1421](https://github.com/clangd/clangd/issues/1421) |
| Primitive token type                  | Supported   |                                                             |
| Bracket token types                   | Unsupported |                                                             |

### Comments

line, block and doc comments, including multiline blocks

```cpp
// A line comment.
/* a one-line block comment */
/*
 * a block comment
 * spanning several lines
 */
/// a doc comment
int after_comments = 0;

/* first
second */ int after_block = 1;
```

### Literals

numbers, characters and strings, including raw strings

```cpp
int decimal = 42;
int hexadecimal = 0xFF;
double floating = 3.14;
char letter = 'x';
const char* text = "hello";
const char* raw = R"(no "escapes" in here)";
int after_raw = 1;

const char* multiline = R"(line1
line2
)"; int after_closing = 2;
```

### Keywords

Including alternative operator spellings and the contextual `final` / `override`

```cpp
bool logic(bool a, bool b) {
    return a and b or not a;
}

struct Base {
    virtual void act();
    virtual ~Base();
};

struct Leaf final : Base {
    void act() override;
};

struct Last : Base {
    void act() final;
};
```

### Preprocessor directives

`#if` chains keep directive kinds; disabled branches keep lexical kinds; pragma arguments stay plain

```cpp
int before_conditional = 0;

#if 0
int disabled_branch;
#else
int enabled_branch = 1;
#endif

#define FLAG
#ifdef FLAG
int flagged = 2;
#endif

#pragma pack(1)

#
#define STRINGIZE(x) #x
const char* stringized = STRINGIZE(abc);
```

### Inactive regions

Tokens in untaken branches keep their lexical kinds and carry the `inactive` modifier; unclassified tokens become plain `identifier` carriers, so even a lone `}` line dims

```cpp
int before = 0;

#if 0
int simple = 1;
bare identifiers;
call(arg);
"string in dead code";
// comment inside
#ifdef NESTED
int deeper = 2;
#endif
int tail = 3;
#endif

#if defined(MISSING)
first_branch;
#elif 0
elif_branch;
#else
int taken = 4;
#endif

#if 0
void edge() {
    inner(5);
}
#endif
```

### Header names

Quoted and angled `#include` filenames, including the split `# include` form

```cpp
#include "inc/angled.h"
#include <angled.h>
# include "inc/angled.h"

int after_includes = 0;
```

### Inactive regions at the top of a file

Untaken branches among the leading directives dim the same way

```cpp
#define KEEP 1
#if 0
#define DEAD 2
#endif

int after = KEEP;
```

### Literal prefixes and suffixes

Encoding prefixes, type suffixes, digit separators and UDL suffixes as distinct tokens

```cpp
using size_type = decltype(sizeof(0));
constexpr size_type operator""_kb(unsigned long long n) {
    return n * 1024;
}

auto wide = L"wide string";
auto utf8 = u8"utf-8 string";
auto hex = 0xFF;
auto binary = 0b1010;
auto unsigned_suffix = 42u;
auto float_suffix = 3.14f;
auto separators = 1'000'000;
auto udl = 4_kb;
```

### Escape sequences

Highlighted distinctly inside string and character literals

```cpp
const char* escaped = "hello\nworld";
char hex_escape = '\x41';
```

### Declarator vs operator disambiguation

`*`, `&`, `&&` as declarators vs arithmetic/logical operators

```cpp
int value = 1;
int* pointer = &value;
int& reference = value;
int product = value * value;
int masked = value & 1;
```

### Primitive token type

A distinct kind for built-in types instead of plain `keyword`

```cpp
int number = 0;
float ratio = 0.5f;
void act();
unsigned long long wide_number = 0;
__int128 extended_int = 0;
_Float16 extended_float = 0;
```

### Bracket token types

Matching `()`, `[]`, `{}`, `<>` pairs as distinct kinds

```cpp
template <typename T>
struct Grid {
    T cells[4];
};

Grid<int> grid{{1, 2, 3, 4}};

int first(Grid<int>& grid) {
    return grid.cells[0];
}
```

<!-- END GENERATED ITEMS -->

## Declarations & References

Names classified by the declaration they define or reference.

<!-- BEGIN GENERATED ITEMS: declarations_references -->

| Capability                                 | Status    | Issues                                                                                                               |
| ------------------------------------------ | --------- | -------------------------------------------------------------------------------------------------------------------- |
| Namespaces                                 | Supported |                                                                                                                      |
| Types                                      | Supported |                                                                                                                      |
| Functions and methods                      | Supported |                                                                                                                      |
| Variables                                  | Supported |                                                                                                                      |
| Templates                                  | Supported |                                                                                                                      |
| Concepts                                   | Supported |                                                                                                                      |
| Labels                                     | Supported |                                                                                                                      |
| Structured bindings                        | Supported |                                                                                                                      |
| Member initializer lists                   | Supported | [clangd#122](https://github.com/clangd/clangd/issues/122)                                                            |
| Using declarations                         | Supported | [clangd#2619](https://github.com/clangd/clangd/issues/2619)                                                          |
| Lambda init-captures                       | Supported | [clangd#868](https://github.com/clangd/clangd/issues/868)                                                            |
| `sizeof...`                                | Supported | [clangd#213](https://github.com/clangd/clangd/issues/213)                                                            |
| `using enum`                               | Supported | [clangd#1283](https://github.com/clangd/clangd/issues/1283)                                                          |
| Deduction guides                           | Supported |                                                                                                                      |
| Explicit instantiation                     | Supported | [clangd#316](https://github.com/clangd/clangd/issues/316)                                                            |
| Dependent names                            | Partial   | [clangd#154](https://github.com/clangd/clangd/issues/154), [clangd#297](https://github.com/clangd/clangd/issues/297) |
| Variable templates                         | Supported |                                                                                                                      |
| Out-of-line member definitions             | Supported |                                                                                                                      |
| Alias templates                            | Supported |                                                                                                                      |
| Template template parameters               | Supported |                                                                                                                      |
| Lambda captures                            | Supported |                                                                                                                      |
| Range-based for                            | Supported |                                                                                                                      |
| Enum underlying types                      | Supported |                                                                                                                      |
| Friend declarations                        | Supported |                                                                                                                      |
| Dependent using declarations               | Partial   |                                                                                                                      |
| Function explicit instantiation directives | Partial   | [llvm#191658](https://github.com/llvm/llvm-project/issues/191658)                                                    |
| Variable explicit instantiation directives | Partial   | [llvm#191658](https://github.com/llvm/llvm-project/issues/191658)                                                    |
| Explicit instantiation member bodies       | Supported |                                                                                                                      |

### Namespaces

definitions, references, nested namespaces and namespace aliases

```cpp
namespace demo {
namespace inner {
int value = 1;
}
}

namespace demo::inner::more {}

namespace alias = demo::inner;

int use_alias = alias::value;
```

### Types

class, struct, union, enum and type aliases, at definitions and references

```cpp
class Widget {};
struct Point {};
union Storage {
    int i;
    float f;
};
enum Flags { FlagA };
enum class Mode { Fast };

typedef Point PointAlias;
using WidgetAlias = Widget;

Widget* make_widget();
PointAlias origin;
Mode current = Mode::Fast;
```

### Functions and methods

declarations, definitions and call sites

```cpp
int twice(int value);

int twice(int value) {
    return value * 2;
}

struct Machine {
    void start();
    static void reset();
};

void drive(Machine machine) {
    machine.start();
    Machine::reset();
    int four = twice(2);
}
```

### Variables

globals, locals, parameters, fields and enum members

```cpp
struct Holder {
    int field;
    static int shared;
};

enum class State { Idle };

int global_value = 1;

void touch(int param) {
    int local = param + global_value;
    Holder h;
    h.field = local;
    Holder::shared = h.field;
    State state = State::Idle;
}
```

### Templates

Type and non-type template parameters, with the `templated` modifier on template names

```cpp
template <typename T, int N>
struct Array {
    T data[N];
};

template <typename T>
T identity(T value);

template <typename T>
T identity(T value) {
    return value;
}

Array<int, 4> arr;
int result = identity(3);
```

### Concepts

Definitions and uses as template constraints

```cpp
template <typename T>
concept Small = sizeof(T) <= 4;

template <Small T>
void use_small(T value);

template <typename T>
    requires Small<T>
void require_small(T value);
```

### Labels

`goto` targets and label definitions

```cpp
void retry(bool again) {
    goto done;
done:
    if (again) {
        goto done;
    }
}
```

### Structured bindings

Binding names at definition and use

The opening `[` deliberately carries no token; only the binding names
themselves are highlighted.

```cpp
struct Pair {
    int first, second;
};

void unpack() {
    auto [a, b] = Pair{1, 2};
    int sum = a + b;
}
```

### Member initializer lists

Initialized fields highlighted as fields

```cpp
struct Widget {
    int width;
    int height;

    Widget(int w, int h) : width(w), height(h) {}
};
```

### Using declarations

The introduced name keeps its target's kind

```cpp
namespace tools {
inline int helper() {
    return 1;
}
struct Gadget {};
}

using tools::helper;
using tools::Gadget;

int used = helper();
Gadget gadget;
```

### Lambda init-captures

The captured name highlighted as a variable

```cpp
int compute();

auto fn = [val = compute()] {
    return val;
};
```

### `sizeof...`

The pack parameter keeps its type-parameter token

```cpp
template <typename... Ts>
constexpr auto count = sizeof...(Ts);
```

### `using enum`

The enum name highlighted at the using site

```cpp
enum class Color { Red };

void paint() {
    using enum Color;
    auto c = Red;
}
```

### Deduction guides

The guide name and the guided template highlighted

```cpp
template <typename T>
struct Vec {
    template <typename It>
    Vec(It first, It last);
};

template <typename It>
Vec(It, It) -> Vec<int>;
```

### Explicit instantiation

The instantiated template name and its written template arguments highlighted, on the extern declaration and the definition alike

```cpp
struct Widget {};

template <typename T>
struct Holder {
    T value;
};

extern template struct Holder<Widget>;

template struct Holder<Widget>;
```

### Dependent names

Resolved through the primary template where one is known

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration and currently get no token;
heuristic coloring for such names remains open.

```cpp
template <typename T>
struct Box {
    using value_type = int;
    static void reset();
    int size() const;
};

template <typename T>
void resolved(Box<T> box) {
    typename Box<T>::value_type item;
    Box<T>::reset();
    box.size();
}

template <typename T>
void unresolved(T value) {
    typename T::value_type item;
    T::reset();
    value.size();
}
```

### Variable templates

declarations, definitions, partial and full specializations

```cpp
template <typename T, typename U>
extern int pair_value;

template <typename T, typename U>
int pair_value = 2;

template <typename T>
extern int pair_value<T, int>;

template <typename T>
int pair_value<T, int> = 4;

template <>
int pair_value<int, int> = 5;
```

### Out-of-line member definitions

Qualified names keep method kinds and modifiers

```cpp
struct Gauge {
    int read() const;
    static void reset();
};

int Gauge::read() const {
    return 0;
}

void Gauge::reset() {}
```

### Alias templates

The alias name carries the type kind and the `templated` modifier

```cpp
template <typename T>
using Ptr = T*;

template <typename T>
struct Box {};

template <typename T>
using BoxPtr = Box<T>*;

Ptr<int> pointer = nullptr;
```

### Template template parameters

Declared and used as types

```cpp
template <typename T>
struct Holder {};

template <template <typename> class Container, typename T>
struct Adaptor {
    Container<T> value;
};

Adaptor<Holder, int> adaptor;
```

### Lambda captures

by-copy and by-reference captures reference the captured variable; `this` stays a keyword

```cpp
struct S {
    int field;

    int compute() {
        int local = 1;
        auto by_copy = [local, this] {
            return local + this->field;
        };
        auto by_reference = [&local] {
            return local;
        };
        return by_copy() + by_reference();
    }
};
```

### Range-based for

The loop variable at definition and use

```cpp
struct List {
    int* begin();
    int* end();
};

void iterate(List items) {
    for (auto& item : items) {
        item = 0;
    }
}
```

### Enum underlying types

The enum-base reference keeps its type kind

```cpp
using Byte = unsigned char;

enum class Flags : Byte { A, B };

Flags flags = Flags::A;
```

### Friend declarations

Befriended names resolve to their targets; inline friends define

```cpp
struct Widget;
void ping();

struct Host {
    friend struct Widget;
    friend void ping();
    friend void inline_friend() {}
};
```

### Dependent using declarations

`using T::name` in a template body

The introduced name and its uses currently get no token; the reserved
dependent-name modifier is not emitted yet.

```cpp
template <typename T>
struct Derived : T {
    using T::value;

    int use() {
        return value;
    }
};
```

### Function explicit instantiation directives

Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments and the parameter types

```cpp
struct Widget {};

template <typename T>
void convert(T value) {}

extern template void convert<Widget>(Widget);

template void convert<Widget>(Widget);
```

### Variable explicit instantiation directives

Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments, even the declarator's type

```cpp
struct Widget {};

template <typename T>
T zero = T();

extern template Widget zero<Widget>;

template Widget zero<Widget>;
```

### Explicit instantiation member bodies

A dependent name paints as its actual resolution: agreeing kinds keep the modifiers all instantiations share, disagreeing kinds paint a conflict

```cpp
struct A {
    static void hit();
};

struct B {
    static int hit;
};

struct C {
    void hit();
};

template <typename T>
struct D {
    void go() {
        (void)T::hit;
    }
};

template struct D<A>;
template struct D<B>;

template <typename T>
struct E {
    void probe(T t) {
        t.hit();
    }
};

template struct E<A>;
template struct E<C>;
```

<!-- END GENERATED ITEMS -->

## Modules

<!-- BEGIN GENERATED ITEMS: modules -->

| Capability                           | Status    | Issues |
| ------------------------------------ | --------- | ------ |
| Module declarations                  | Supported |        |
| Module partitions                    | Supported |        |
| `module` and `import` as identifiers | Supported |        |

### Module declarations

The contextual `module` keyword, dotted module names and the private fragment

```cpp
module;

export module demo.core;

export int exported_value = 1;

module :private;

int private_value = 2;

#if 0
module :private;
#endif
```

### Module partitions

Partition names in the module declaration

```cpp
export module demo.core:part;

export int partition_value = 1;
```

### `module` and `import` as identifiers

Contextual keywords keep their semantic kinds outside module declarations

```cpp
void f() {
    struct module {};
    module m;
    int import = 1;
    int module = 2;
}
```

<!-- END GENERATED ITEMS -->

## Token Modifiers

<!-- BEGIN GENERATED ITEMS: token_modifiers -->

| Capability                    | Status      | Issues                                                      |
| ----------------------------- | ----------- | ----------------------------------------------------------- |
| Declaration vs definition     | Supported   |                                                             |
| Static                        | Supported   |                                                             |
| Readonly                      | Supported   |                                                             |
| Virtual and abstract          | Supported   |                                                             |
| Deprecated                    | Supported   |                                                             |
| Default library               | Supported   |                                                             |
| Scope modifiers               | Unsupported | [clangd#352](https://github.com/clangd/clangd/issues/352)   |
| Mutable reference and pointer | Unsupported | [clangd#839](https://github.com/clangd/clangd/issues/839)   |
| Deduced                       | Unsupported |                                                             |
| User-defined operators        | Unsupported | [clangd#1521](https://github.com/clangd/clangd/issues/1521) |

### Declaration vs definition

The modifier distinguishes the two

```cpp
int measure(int value);

int measure(int value) {
    return value;
}

struct Sensor;

struct Sensor {};
```

### Static

class-level members and static locals

```cpp
struct Counter {
    static int total;
    static void bump();
    int current;
};

void count() {
    static int calls = 0;
    Counter::bump();
    Counter::total = calls;
}
```

### Readonly

Const and constexpr values, const methods and enum members

Readonly is currently value-based: a pointer to const counts as
readonly even though the pointer itself can change.

```cpp
enum class Level { High };

const int limit = 10;
constexpr int bound = 4;

struct Gauge {
    int read() const;
    void write(int value);
};

void probe(const int& in, const int* pointee_const, int* const self_const) {
    Gauge gauge;
    gauge.read();
    gauge.write(limit);
}
```

### Virtual and abstract

Virtual methods, pure virtual methods and abstract classes

```cpp
struct Shape {
    virtual int area();
    virtual int perimeter() = 0;
    virtual ~Shape();
};

struct Square : Shape {
    int perimeter() override;
};

int measure(Shape& shape) {
    return shape.area() + shape.perimeter();
}
```

### Deprecated

`[[deprecated]]` declarations and their uses

```cpp
[[deprecated("use next_api")]] void old_api();
void next_api();

void migrate() {
    old_api();
}
```

### Default library

Symbols declared in system headers

```cpp
int before_includes = 0;

#include <syslib.h>

int used = system_helper();
```

### Scope modifiers

function, class, file and global scope

```cpp
int global_scope;
static int file_scope;

struct Foo {
    int class_scope;

    void bar() {
        int function_scope = 0;
    }
};
```

### Mutable reference and pointer

Arguments passed by non-const reference or pointer

```cpp
void modify(int& out);
void modify_through(int* out);
void inspect(const int& in);

void run() {
    int value = 0;
    modify(value);
    modify_through(&value);
    inspect(value);
}
```

### Deduced

Mark deduced types such as `auto` and `decltype`

```cpp
auto deduced_int = 1;
decltype(deduced_int) same_type = 2;
```

### User-defined operators

Distinguish overloaded operators from built-in ones

```cpp
struct Vec {
    Vec operator+(const Vec& other) const;
};

Vec add(Vec a, Vec b) {
    return a + b;
}

int add(int a, int b) {
    return a + b;
}
```

<!-- END GENERATED ITEMS -->

## Conflict & Ambiguity

C++ allows structurally different entities to share one name. When a single
written name refers to entities of different kinds at once, no single token
type is correct; such names receive the dedicated **conflict** token type,
which clients typically display in a neutral color.

<!-- BEGIN GENERATED ITEMS: conflict_ambiguity -->

| Capability              | Status    | Issues |
| ----------------------- | --------- | ------ |
| Type vs function        | Supported |        |
| Type vs variable        | Supported |        |
| Same-kind overload sets | Supported |        |
| Injected class name     | Supported |        |

### Type vs function

A name naming both renders as `conflict`

```cpp
namespace shop {
struct Widget {};
void Widget();
}

using shop::Widget;
```

### Type vs variable

A name naming both renders as `conflict`

```cpp
namespace mixed {
struct Thing {};
int Thing;
}

using mixed::Thing;
```

### Same-kind overload sets

A name naming only functions is no conflict

```cpp
namespace ops {
void apply();
void apply(int level);
}

using ops::apply;

void run() {
    apply();
    apply(1);
}
```

### Injected class name

The class name used as a constructor call inside the class

The written name renders as the class; the constructor reference it
implies paints nothing extra — the `(` stays token-free.

```cpp
struct Widget {
    Widget(int size);

    Widget create() {
        return Widget(42);
    }
};
```

<!-- END GENERATED ITEMS -->

## Token Correctness

Shapes clice pins deliberately, including issues clangd got wrong.

<!-- BEGIN GENERATED ITEMS: token_correctness -->

| Capability                                | Status    | Issues                                                                                                                                                                              |
| ----------------------------------------- | --------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Constructors and destructors              | Supported | [clangd#1509](https://github.com/clangd/clangd/issues/1509), [clangd#2078](https://github.com/clangd/clangd/issues/2078), [clangd#872](https://github.com/clangd/clangd/issues/872) |
| Anonymous parameters                      | Supported |                                                                                                                                                                                     |
| Operator names                            | Supported |                                                                                                                                                                                     |
| Destructors of class templates            | Supported |                                                                                                                                                                                     |
| Conversion operators                      | Supported |                                                                                                                                                                                     |
| Pseudo-destructor on a template parameter | Supported |                                                                                                                                                                                     |
| Defaulted and deleted members             | Supported |                                                                                                                                                                                     |

### Constructors and destructors

Method tokens with the constructor/destructor modifier

A destructor name renders as two tokens: the `~` carries the method
kind and the declaration/definition modifiers, the class name after it
stays a reference to the class.

```cpp
struct Session {
    Session();
    ~Session();
};

Session::Session() {}

Session::~Session() {}

void destroy(Session* session) {
    session->~Session();
}
```

### Anonymous parameters

Unnamed parameters produce no tokens

The punctuation after an unnamed parameter's type stays token-free.

```cpp
void take_one(int) {}
void take_two(int, char* c) {}
```

### Operator names

The `operator` keyword and call-site punctuation stay plain

An operator's written name is keyword plus punctuation, so no name
token is painted: `operator` keeps its keyword classification and
call sites emit nothing on the operator symbol.

```cpp
struct Value {
    Value& operator=(const Value& other);
    Value operator+(const Value& other) const;
};

void combine(Value a, Value b) {
    a = b;
    Value c = a + b;
}
```

### Destructors of class templates

The `~` shape holds under templates

```cpp
template <typename T>
struct Holder {
    ~Holder();
};

template <typename T>
Holder<T>::~Holder() {}
```

### Conversion operators

Written as keywords, converting uses paint nothing extra

```cpp
struct Ratio {
    operator double() const;
    explicit operator bool() const;
};

double to_double(Ratio ratio) {
    if (ratio) {
        return ratio;
    }
    return double(ratio);
}
```

### Pseudo-destructor on a template parameter

The `~` paints nothing; the type name keeps its kind

```cpp
template <typename T>
void reset(T* value) {
    value->~T();
}
```

### Defaulted and deleted members

special-member names keep their definition tokens

```cpp
struct Session {
    Session() = default;
    Session(const Session&) = delete;
    ~Session() = default;
};
```

<!-- END GENERATED ITEMS -->

## Attributes

<!-- BEGIN GENERATED ITEMS: attributes -->

| Capability      | Status      | Issues                                                      |
| --------------- | ----------- | ----------------------------------------------------------- |
| Attribute names | Unsupported | [clangd#2209](https://github.com/clangd/clangd/issues/2209) |

### Attribute names

Standard and vendor attributes, and expressions inside them

```cpp
[[nodiscard]] int compute();
[[deprecated("use v2")]] void old_func();
[[maybe_unused]] int counter = 0;

struct [[gnu::packed]] Packed {};
```

<!-- END GENERATED ITEMS -->

## Macros

Tokens inside macro definition bodies keep their lexical kinds; highlighting
them from their expansions belongs to a future expansion-preview feature.

<!-- BEGIN GENERATED ITEMS: macros -->

| Capability                          | Status      | Issues                                                      |
| ----------------------------------- | ----------- | ----------------------------------------------------------- |
| Macro definition and expansion      | Supported   |                                                             |
| Expansion sites and arguments       | Supported   |                                                             |
| Object-like vs function-like macros | Unsupported | [clangd#2649](https://github.com/clangd/clangd/issues/2649) |

### Macro definition and expansion

```cpp
#define SQUARE(x) ((x) * (x))

[[maybe_unused]] static int squared = SQUARE(4);
```

### Expansion sites and arguments

Expansion names are macros, written arguments keep their semantics, definition bodies stay lexical

```cpp
int value = 1;

#define ID(x) x
#define CALL helper()

void helper();

int copied = ID(value);

void run() {
    CALL;
}
```

### Object-like vs function-like macros

Distinct highlighting for the two forms

```cpp
#define MAX_SIZE 1024
#define CHECK(x) ((x) ? 1 : 0)

int checked = CHECK(MAX_SIZE);
```

<!-- END GENERATED ITEMS -->

## Other Known Gaps

Curated issues without a fixture yet:

- `auto` parameters must not be highlighted as template type parameters
  ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- Nested name specifier in a pointer-to-member should get a token
  ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- `::new` should keep the `new` keyword highlighted
  ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- `co_yield` / `co_await` lose highlighting when the coroutine return type is
  a template ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- Token modifiers should apply to operands of overloaded operators
  ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- Dependent template names (`obj.template get<int>()`), members imported from
  a dependent base via `using`, and dependent names with mixed-kind overload
  sets ([clangd#484](https://github.com/clangd/clangd/issues/484),
  [clangd#686](https://github.com/clangd/clangd/issues/686),
  [clangd#1057](https://github.com/clangd/clangd/issues/1057))

## Inactive Code Regions

Every token inside an untaken preprocessor branch carries the `inactive`
modifier while keeping its lexical kind, so editors dim the region by
styling the modifier without losing the syntax colors underneath. Tokens
without a classification in dead code — bare identifiers and plain
punctuation — are emitted as the unstyled `identifier` type, giving the
whole region token coverage. The clice VS Code extension renders the
regions dimmed out of the box; other editors style the modifier directly
(e.g. `@lsp.mod.inactive` in Neovim).

- [x] Dim inactive preprocessor branches ([clangd#132](https://github.com/clangd/clangd/issues/132))
- [x] Correct inactive boundaries with `#elif` chains ([clangd#602](https://github.com/clangd/clangd/issues/602))
- [x] Preserve syntax highlighting within inactive regions ([clangd#1664](https://github.com/clangd/clangd/issues/1664))
- [x] Keep inactive regions distinct from comments ([clangd#1545](https://github.com/clangd/clangd/issues/1545))
- [ ] Unreachable code dimming ([clangd#1828](https://github.com/clangd/clangd/issues/1828))

## Format String Highlighting

- [ ] `std::format` / `std::print` placeholder highlighting ([clangd#1709](https://github.com/clangd/clangd/issues/1709))
- [ ] Highlight invalid format specifiers as errors

## Protocol Support

- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [x] UTF-16 delta-encoded token positions
- [ ] Range-based semantic tokens (`textDocument/semanticTokens/range`) — only
      compute tokens for the visible viewport, critical for large files
- [ ] Delta updates (`textDocument/semanticTokens/full/delta`) — send only
      changes since the previous response
