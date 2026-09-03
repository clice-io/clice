# 内联提示

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/inlay_hint/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 为代码中未显式给出的信息渲染内联标注：调用处的参数名、推导出的类型，以及按位置进行聚合初始化时对应的字段名。提示类别可以通过 `[inlay_hints]` 配置节单独开关；下面各节介绍默认开启的类别。

## 参数名提示

<!-- BEGIN GENERATED ITEMS: parameter_hints -->

| 能力                        | 状态     | 问题                                                                                                                     |
| --------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------ |
| 参数名提示                  | 支持     |                                                                                                                          |
| 提示抑制                    | 支持     | [clangd#1877](https://github.com/clangd/clangd/issues/1877)                                                              |
| Setter 与内建函数的提示抑制 | 支持     |                                                                                                                          |
| 可变引用标记                | 支持     | [clangd#1123](https://github.com/clangd/clangd/issues/1123)                                                              |
| 转发解析                    | 支持     | [clangd#2324](https://github.com/clangd/clangd/issues/2324)                                                              |
| 来自定义的名称              | 支持     |                                                                                                                          |
| 函数指针与调用运算符        | 支持     | [clangd#1734](https://github.com/clangd/clangd/issues/1734), [clangd#1742](https://github.com/clangd/clangd/issues/1742) |
| 推导 `this`                 | 支持     | [clangd#1777](https://github.com/clangd/clangd/issues/1777)                                                              |
| 依赖调用                    | 支持     |                                                                                                                          |
| 未展开的参数包              | 支持     |                                                                                                                          |
| 调用处的宏                  | 支持     | [clangd#2620](https://github.com/clangd/clangd/issues/2620)                                                              |
| 隐式构造函数调用            | 支持     |                                                                                                                          |
| 伪对象表达式                | 支持     |                                                                                                                          |
| 显式实例化                  | 支持     | [clangd#1034](https://github.com/clangd/clangd/issues/1034)                                                              |
| 宽松名称匹配                | 部分支持 | [clangd#2248](https://github.com/clangd/clangd/issues/2248)                                                              |
| 继承构造函数                | 部分支持 | [clangd#1364](https://github.com/clangd/clangd/issues/1364)                                                              |
| 匿名参数                    | 支持     |                                                                                                                          |
| 运算符与字面量              | 支持     |                                                                                                                          |
| 构造函数实参中的参数包      | 部分支持 |                                                                                                                          |

### 参数名提示

函数调用点和构造函数调用中的实参名称

```cpp
void draw(int width, int height);

struct Point {
    Point(int x, int y);
    Point(const Point& other);
    Point(Point&& other);
};

void use() {
    draw(10, 20);
    Point p(1, 2);
    Point q{3, 4};
    // Copy and move constructors stay quiet; a temporary's own braces
    // still hint (the outer prvalue construction is elided anyway).
    Point r(p);
    Point m(Point{5, 6});
    Point s(static_cast<Point&&>(r));
}
```

### 提示抑制

本身已写明参数名的实参，以及 `/*name=*/` 注释

```cpp
void draw(int width, int height);

void use() {
    int width = 5;
    int h = 2;
    // `width` matches the parameter spelling: only `height:` hints.
    draw(width, h);
    // An inline comment naming the parameter serves the same purpose;
    // a comment naming something else does not.
    draw(/*width=*/1, /*height=*/2);
    draw(/*margin=*/6, 7);
}

struct Sizes {
    static int width;
    int height;

    void member() {
        // A bare member access spells the parameter name: suppressed.
        draw(5, height);
    }
};

void qualified(Sizes s) {
    // A qualified name is not a plain spelling match.
    draw(Sizes::width, 3);
    // Neither is an access through a written base object.
    draw(4, s.height);
}
```

### Setter 与内建函数的提示抑制

`setX(x)` 以及 `std::move`/`std::forward` 的实参不显示提示

```cpp
namespace std {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
struct remove_reference<T&> {
    using type = T;
};

template <typename T>
struct remove_reference<T&&> {
    using type = T;
};

template <typename T>
constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

template <typename T>
constexpr typename remove_reference<T>::type&& move(T&& t) noexcept;

}  // namespace std

struct Config {
    void setWidth(int width);
    void set_height(int height);
    // The parameter carries extra information beyond the setter name, so
    // it still hints.
    void setTimeout(int timeout_millis);
};

void consume(int&& sink);

// The three-argument algorithm form of std::move is a real call whose
// parameters deserve hints; only the single-argument cast stays bare.
namespace std {

template <typename T>
T* move(T* first, T* last, T* result);

}  // namespace std

void use(Config& config) {
    config.setWidth(3);
    config.set_height(4);
    config.setTimeout(5);
    int value = 1;
    consume(std::move(value));
    int buffer[4];
    std::move(buffer, buffer + 2, buffer + 2);
}
```

### 可变引用标记

`&` 标记通过非 const 左值引用传递的实参

```cpp
void mutate(int& value);
void observe(const int& value);
void take(int&& value);

void use() {
    int v = 0;
    mutate(v);
    observe(v);
    take(static_cast<int&&>(v));
}
```

### 转发解析

经包装器转发的参数包会解析为目标函数的参数名

```cpp
namespace std {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

}  // namespace std

void target(int first, int second);

template <typename... Args>
void wrap(Args&&... args) {
    target(std::forward<Args>(args)...);
}

// A plain pass-through works without std::forward as well.
void sink(int a, int b, int c);

template <typename... Ts>
void call_with(Ts... ts) {
    sink(ts...);
}

// Forwarding also resolves through packs sandwiched between fixed
// head and tail arguments.
int accumulate(int, int b, double);

template <typename... Args>
int head_tail(int a, Args&&... args) {
    return accumulate(1, std::forward<Args>(args)..., 1.0);
}

template <typename... Args>
int chain(Args&&... args) {
    return head_tail(std::forward<Args>(args)...);
}

void use() {
    wrap(1, 2);
    call_with(1, 2, 3);
    chain(32, 42);
}
```

### 来自定义的名称

声明中未命名的参数采用定义中的名称；前导下划线会被去掉

```cpp
void resize(int, int);

void fill(int _value, int __count);

int scale(int good);

void use() {
    resize(800, 600);
    fill(1, 2);
    // When both name their parameter, the declaration wins.
    scale(7);
}

void resize(int width, int height) {}

int scale(int bad) {
    return bad;
}
```

### 函数指针与调用运算符

间接调用仍会显示其参数名

```cpp
struct Callback {
    void operator()(int status, int detail) const;
};

void (*handler)(int status, const char* message);

void use() {
    Callback cb;
    cb(1, 2);
    cb.operator()(3, 4);
    handler(0, "ok");
    auto cmp = [](int lhs, int rhs) { return lhs < rhs; };
    cmp(1, 2);
}
```

### 推导 `this`

显式对象形参不会显示提示（C++23）

```cpp
struct Widget {
    void resize(this Widget& self, int width, int height);
};

void use() {
    Widget w;
    w.resize(800, 600);
}
```

### 依赖调用

即使仅能在模板内部确定被调用方，仍会显示参数名

候选项按实参数量匹配；只有筛选后剩下唯一候选项时才会显示参数名，因此，如果一次调用仍可能匹配多个重载，就不会猜测，而是不显示提示。

```cpp
template <typename T>
void apply(T scale);

template <typename T>
struct Holder {
    void member(T item);
    static void static_member(T slot);
};

void overload(int value);
void overload(double value);

template <typename T>
struct Runner {
    void run(Holder<T> holder, T value) {
        apply(value);
        holder.member(value);
        Holder<T>::static_member(value);
        // Several overloads remain viable: no hint.
        overload(T{});
    }
};
```

### 未展开的参数包

显式写出的参数包展开会破坏实参的一一对应关系，并停止显示提示

```cpp
void plot(int x, int y, int z);

template <typename... Ts>
void relay(Ts... ts) {
    // `ts...` may instantiate to any number of arguments.
    plot(0, ts...);
}

void use() {
    // The outer call still resolves through pack forwarding: 1 and 2 land
    // in plot's y and z.
    relay(1, 2);
}
```

### 调用处的宏

以宏形式写出的实参会显示提示；在宏体内部生成的调用则不会

```cpp
void report(double value);
void plot(double x, double y);
int check(int status);

#define PI 3.14
#define CALL_REPORT() report(2.71)
#define PAIR 1.0, 2.0
#define ASSERT(expr) if(!(expr)) {}

void use() {
    // An object-like macro is still one written argument.
    report(PI);
    // The call only exists inside the macro body.
    CALL_REPORT();
    // One macro covering several arguments has no place to anchor.
    plot(PAIR);
    // Code written as a macro argument keeps its hints.
    ASSERT(check(42) == 0);
}
```

### 隐式构造函数调用

代码中未显式写出的转换本身不会产生提示

```cpp
struct Seconds {
    Seconds(int raw);
};

void wait(Seconds);
void hold(Seconds duration);

Seconds use() {
    // The implicit Seconds(5) must not surface `raw:`.
    wait(5);
    // The written call still hints its own parameter.
    hold(6);
    // Nor does the conversion in a return statement.
    return 7;
}
```

### 伪对象表达式

MS 属性访问不显示提示；显式写出的下标访问会保留访问器的参数名

```cpp
int printf(const char* Format, ...);

struct State {
    __declspec(property(get = GetX, put = PutX)) int x[];
    int GetX(int row, int column);
    void PutX(int value);

    // The syntactic form is a binary operator: no `value:` hint on `y`.
    void Work(int y) {
        x = y;
    }
};

int use() {
    State s;
    // The semantic form of __builtin_dump_struct calls printf; none of it
    // is written here.
    __builtin_dump_struct(&s, printf);
    printf("%d", 42);
    // Property subscripts read best with the accessor's parameter names.
    return s.x[1][2];
}
```

### 显式实例化

显式实例化定义不会添加重复提示，而其中显式写出的模板实参会正常显示提示

```cpp
template <typename T>
void apply(T value) {}

template void apply<int>(int value);

void use() {
    apply(42);
}

int measure(int amount);

template <typename T>
struct Box {};

template struct Box<decltype(measure(7))>;
```

### 宽松名称匹配

`aParam` 尚不能让写作 `param` 的实参不显示提示

```cpp
void draw(int aParam);

void use() {
    int param = 3;
    // Ideally the near-match would suppress the hint; today it still shows.
    draw(param);
}
```

### 继承构造函数

`using Base::Base` 引入的构造函数调用会丢失参数名

```cpp
struct Base {
    Base(int width);
};

struct Derived : Base {
    using Base::Base;
};

// No `width:` hint yet.
Derived d(7);
```

### 匿名参数

没有名称可提示，不过可变引用仍会标记 `&`

```cpp
void value_sink(int);
void ref_sink(int&);
void const_ref_sink(const int&);
void rvalue_sink(int&&);

void use() {
    int v = 0;
    value_sink(1);
    // Only the `&` marker survives without a name.
    ref_sink(v);
    const_ref_sink(v);
    rvalue_sink(2);
}
```

### 运算符与字面量

运算符语法和用户定义字面量不显示提示；成员初始化器和默认成员初始化器会显示提示

```cpp
struct S {
    S(int param);
};

void operator+(S lhs, S rhs);

long double operator""_w(long double param);

struct Holder {
    S member;
    S defaulted{3};
    Holder() : member(42) {}
};

void use() {
    S a(1);
    S b(2);
    a + b;
    1.2_w;
}
```

### 构造函数实参中的参数包

外层调用可解析；展开内部的提示仍未实现

```cpp
struct Foo {
    Foo();
    Foo(int x);
};

void consume(Foo a, int b);

template <typename... Args>
void relay(Args... args) {
    consume(args...);
}

template <typename... Args>
void construct(Args... args) {
    // The written Foo{args...} and the literal after it get no hints yet.
    consume(Foo{args...}, 1);
}

void use() {
    relay(Foo{}, 42);
    relay(42, 42);
    construct(42);
}
```

<!-- END GENERATED ITEMS -->

## 类型提示

<!-- BEGIN GENERATED ITEMS: type_hints -->

| 能力                              | 状态     | 问题                                                                                                                     |
| --------------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------ |
| 类型由 `auto` 推导的变量          | 支持     |                                                                                                                          |
| 类型语法糖与长度限制              | 支持     | [clangd#1298](https://github.com/clangd/clangd/issues/1298), [clangd#1357](https://github.com/clangd/clangd/issues/1357) |
| 结构化绑定（structured bindings） | 支持     |                                                                                                                          |
| Lambda                            | 支持     | [clangd#1163](https://github.com/clangd/clangd/issues/1163)                                                              |
| 推导出的返回类型                  | 支持     |                                                                                                                          |
| `decltype` 写法                   | 支持     |                                                                                                                          |
| `auto` 参数                       | 支持     |                                                                                                                          |
| 显式写出的初始化器                | 部分支持 | [clangd#1749](https://github.com/clangd/clangd/issues/1749)                                                              |
| 依赖型 `auto`                     | 部分支持 | [clangd#2275](https://github.com/clangd/clangd/issues/2275)                                                              |
| 作用域省略                        | 支持     |                                                                                                                          |
| 元组协议绑定                      | 支持     |                                                                                                                          |
| 已实例化的模板                    | 部分支持 | [clangd#2275](https://github.com/clangd/clangd/issues/2275)                                                              |

### 类型由 `auto` 推导的变量

提示显示变量的完整类型，包括限定符。

```cpp
int make();

void use() {
    auto value = make();
    const auto& ref = value;
    auto* ptr = &value;
}
```

### 类型语法糖与长度限制

别名保留原有写法；类型过长时，回退到带语法糖的名称。

```cpp
using Integer = int;

Integer make_alias();

template <typename A, typename B, typename C>
struct extremely_long_template_name {};

using Compact = extremely_long_template_name<int, char, bool>;

Compact make_compact();

extremely_long_template_name<Integer, Integer, Integer> make_long();

template <typename T, typename U = int>
struct Defaulted {};

Defaulted<float> make_defaulted();

void use() {
    auto aliased = make_alias();
    auto shortened = make_compact();
    // No sugar short enough to fall back to: the hint is dropped.
    auto dropped = make_long();
    // Default template arguments never print.
    auto defaulted = make_defaulted();
}
```

### 结构化绑定（structured bindings）

每个绑定都会提示其规范类型；聚合体本身则不显示提示。

```cpp
struct Pair {
    int first;
    float second;
};

Pair make();

int array[2];

void use() {
    auto [a, b] = make();
    auto [x, y] = array;
}
```

### Lambda

变量、推导出的返回类型和初始化捕获均会显示提示。

```cpp
int compute();

void use() {
    auto callback = [captured = compute()](int x) {
        return x + captured;
    };
    auto bare = [] {
        return 1.5;
    };
}
```

### 推导出的返回类型

`-> T` 显示在参数列表之后，声明中也会显示。

```cpp
auto answer() {
    return 42;
}

auto& ref_answer() {
    static int storage = 0;
    return storage;
}

// A declaration hints once a later definition supplies the deduction; a
// definition-less one stays silent.
auto declared(int x);
auto deducible(int x);

auto deducible(int x) {
    return x + 1;
}

// Written trailing return types need no hint.
auto spelled() -> int;
auto pointer() -> auto* {
    return "text";
}

struct Convertible {
    operator auto() {
        return 42;
    }
};
```

### `decltype` 写法

底层类型显示在写出的 `decltype` 旁边。

```cpp
int source();

decltype(source()) value = 1;

int& ref = value;
// decltype(auto) preserves the reference.
decltype(auto) forwarded = ref;

// Every written decltype spelling hints: declarators, alias targets,
// return types and functional casts.
const decltype(0)& bound = value;

decltype(0) declared();

auto trailing() -> decltype(0);

template <class, class>
struct Wrap;

using Alias = Wrap<decltype(0), float>;

auto constructed = decltype(0){};
```

### `auto` 参数

模板恰好只有一个实例化结果时，会显示推导出的类型。

```cpp
int twice(auto x) {
    return x + x;
}

int result = twice(21);

// A second instantiation makes the deduction ambiguous: no hint.
int measure(auto x) {
    return 1;
}

int a = measure(1);
int b = measure(2.0);

// Packs and parameters after them never hint.
int spread(auto first, auto... rest, auto last) {
    return 0;
}

int c = spread<void*, char, float>(nullptr, 'x', 2.0f, 3);

// Deduplication: a template body hints once across instantiations of the
// same deduced type.
template <typename T>
void body() {
    auto var = 42;
}

template void body<int>();
template void body<float>();
```

### 显式写出的初始化器

强制转换和函数式转换仍会显示冗余提示。

```cpp
int compute();

void use() {
    // The type is already written on the right-hand side; ideally these
    // two hints would be suppressed.
    auto widened = static_cast<long>(compute());
    auto braced = int{42};
}
```

### 依赖型 `auto`

未实例化的模板体内的推导不会显示提示。

```cpp
template <typename T>
void body(T input) {
    // No hint: the deduced type depends on T.
    auto derived = input + 1;
    // A dependence-free initializer still hints normally.
    auto counter = 0;
}
```

### 作用域省略

提示中会省略命名空间限定符，但保留类作用域。

```cpp
namespace outer {
namespace inner {

struct S1 {};
S1 make_s1();
auto x = make_s1();

struct S2 {
    template <typename T>
    struct Nested {};
};

S2::Nested<int> make_nested();
auto y = make_nested();

}  // namespace inner
}  // namespace outer
```

### 元组协议绑定

提示显示规范类型，而不是 `tuple_element<I, T>::type`。

```cpp
struct IntPair {
    int a;
    int b;
};

namespace std {

template <typename T>
struct tuple_size {};

template <>
struct tuple_size<IntPair> {
    constexpr static unsigned value = 2;
};

template <unsigned I, typename T>
struct tuple_element {};

template <unsigned I>
struct tuple_element<I, IntPair> {
    using type = int;
};

}  // namespace std

template <unsigned I>
int get(const IntPair& p) {
    if constexpr(I == 0) {
        return p.a;
    } else {
        return p.b;
    }
}

IntPair make();

auto [x, y] = make();
```

### 已实例化的模板

实例化后的模板体不会在模板模式处重复显示提示；若恰好只有一个实例化结果，依赖型 `auto` 可显示推导出的类型。

```cpp
void take(int first, int second);

template <typename T>
struct Single {
    void reset() {
        take(1, 2);
        // Deducible from the only instantiation, but not yet deduced.
        auto copy = T();
    }
};

template struct Single<char>;

template <typename T>
struct Twice {
    void reset() {
        // No hint: two instantiations deduce contradicting types.
        auto copy = T();
    }
};

template struct Twice<char>;
template struct Twice<int>;
```

<!-- END GENERATED ITEMS -->

## 指派符提示

<!-- BEGIN GENERATED ITEMS: designator_hints -->

| 能力                   | 状态   | 问题                                                        |
| ---------------------- | ------ | ----------------------------------------------------------- |
| 字段和索引指派符       | 支持   | [clangd#2303](https://github.com/clangd/clangd/issues/2303) |
| 嵌套聚合体             | 支持   |                                                             |
| 匿名成员               | 支持   |                                                             |
| 指派符提示抑制         | 支持   |                                                             |
| 仅限聚合类型           | 支持   |                                                             |
| 无效的初始化器         | 支持   |                                                             |
| 圆括号形式的聚合初始化 | 不支持 | [clangd#2540](https://github.com/clangd/clangd/issues/2540) |

### 字段和索引指派符

按位置进行的聚合初始化会显示 `.field=` 和 `[index]=`。

```cpp
struct Point {
    int x;
    int y;
    int z;
};

Point p{1, 2 + 2};

int coordinates[2] = {7, 8};

// Array designators survive dependent-sized members; reserved names are
// skipped rather than printed.
template <typename T, int N>
struct Array {
    T __elements[N];
};

Array<int, 2> pair = {0, 1};
```

### 嵌套聚合体

遇到显式写出的花括号时会递归处理；省略花括号时会展平为 `.outer.inner=`。

```cpp
struct Inner {
    int x;
    int y;
};

struct Outer {
    Inner a;
    Inner b;
};

Outer o{{1, 2}, 3};
```

### 匿名成员

未命名联合体和结构体不会出现在指派符路径中。

```cpp
struct State {
    union {
        struct {
            struct {
                int y;
            };
        } x;
    };
};

State s{42};
```

### 指派符提示抑制

已显式写出的指派符和 `/*name=*/` 注释会使对应的初始化项保持无提示状态

```cpp
struct Point {
    int a;
    int b;
    int c;
    int d;
    int e;
};

// Mixing written designators with positional inits is a C99 extension
// clang accepts with a warning; only the bare `4` needs help.
Point p{/*a=*/1, .c = 2, /* .d = */ 3, 4};
```

### 仅限聚合类型

构造函数调用、拷贝和惯用的零初始化都不会产生指派符提示

```cpp
struct Constructible {
    Constructible(int amount);
};

// A braced constructor call names parameters, not fields.
Constructible built{5};

struct Copyable {
    int x;
};

Copyable original{1};
Copyable duplicate{original};

// The idiomatic `{}` zero-initializer stays quiet.
struct Wide {
    int fields[8];
};

Wide zeroed{};
```

### 无效的初始化器

即使旁边的初始化器编译失败，指派符提示仍会显示

```cpp
// The first initializer deliberately fails to convert.
struct Empty {};

struct Mixed {
    int a;
    int b;
};

void use() {
    Mixed m{Empty(), 1};
}
```

### 圆括号形式的聚合初始化

C++20 `Point(1, 2)` 尚不产生提示

```cpp
struct Point {
    int x;
    int y;
};

Point p(1, 2);
```

<!-- END GENERATED ITEMS -->

## 其他提示类型

<!-- BEGIN GENERATED ITEMS: other_hint_kinds -->

| 能力         | 状态   | 问题                                                        |
| ------------ | ------ | ----------------------------------------------------------- |
| 模板参数提示 | 不支持 | [clangd#2583](https://github.com/clangd/clangd/issues/2583) |
| CTAD 实参    | 不支持 | [clangd#2331](https://github.com/clangd/clangd/issues/2331) |
| 隐式转换提示 | 不支持 | [clangd#2254](https://github.com/clangd/clangd/issues/2254) |

### 模板参数提示

调用点处推导出的和显式指定的模板实参

```cpp
template <typename T, typename U>
T convert(U val);

// Could hint `T: float` next to the explicit argument list.
float converted = convert<float>(42);
```

### CTAD 实参

模板名后推导出的类模板实参

```cpp
template <typename A, typename B>
struct Pair {
    A first;
    B second;
    Pair(A a, B b);
};

// Could hint `<int, double>` after `pair`.
Pair pair(1, 2.5);
```

### 隐式转换提示

显示调用点执行的转换

```cpp
void process(double val);

// Could hint `(double)` before the argument.
void use() {
    process(42);
}
```

<!-- END GENERATED ITEMS -->

## 块结尾提示

默认关闭（`inlay_hints.block_end`）。对于至少跨两行的代码块，clice 会在右花括号后显示其所结束结构的名称——包括函数、类型、命名空间和控制流语句：

```cpp
void Widget::process(const Config& cfg) {
    // ...
} // Widget::process

namespace detail {
    // ...
} // namespace detail

while (running) {
    // ...
} // while running
```

如果条件能以简短文本表示，就会为 `if`/`while`/`switch`/`for` 显示条件摘要；`else if` 链仅提示为 `// if`。超过 60 个字符的标签不会显示。

一个相关设想是用 `#endif` 提示显示与之匹配的条件（[clangd#2487](https://github.com/clangd/clangd/issues/2487)），但该功能尚未实现。

## 默认参数提示

默认关闭（`inlay_hints.default_arguments`）。依赖默认参数的调用点会显示省略的内容，超过类型名长度限制时会缩写：

```cpp
void log(int level, bool flush = true, int repeat = 1);
log(2);
//     ^ , flush: true, repeat: 1
```

## 配置

`clice.toml` 的 `[inlay_hints]` 章节（或通过 `initializationOptions` 提供的同名键）控制所有类别：`enabled`、`parameters`、`deduced_types`、`designators`、`block_end`、`default_arguments` 和 `type_name_limit`。详见[配置指南](../guide/configuration.md#inlay-hints)。配置更改会在服务器重启后生效，无需重新编译。

## 交互行为

- 请求按范围限定：请求范围之外的提示会被丢弃。
- 参数提示锚定在实参左侧；类型提示和指派符提示锚定在声明一侧，通过 LSP padding 标志控制间距，而不嵌入空格。
- 内容完全相同的重复提示（例如模板实例化产生的提示）会合并为一条。

## 其他已知缺口

- 通过 `InlayHintLabelPart` 为缩写类型提示提供可展开的标签部分（[clangd#2269](https://github.com/clangd/clangd/issues/2269)）
- 可点击的类型名——在提示的类型上跳转到定义（[clangd#1535](https://github.com/clangd/clangd/issues/1535)）
- 感知作用域的类型缩写——在 `namespace foo` 内显示 `Bar` 而不是 `foo::Bar`（[clangd#2270](https://github.com/clangd/clangd/issues/2270)）
- 协程返回模板类型时丢失参数提示（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）
