# 代码导航

## 跳转到定义

<!-- BEGIN GENERATED ITEMS: go_to_definition -->

| 能力                                      | 状态   | 问题                                                        |
| ----------------------------------------- | ------ | ----------------------------------------------------------- |
| 跨 TU 跳转到定义                          | 支持   |                                                             |
| 定义与声明在光标处交替切换                | 支持   |                                                             |
| 仅有声明的符号跳转到其声明                | 支持   |                                                             |
| 对 `#include` 指令执行跳转到定义          | 支持   |                                                             |
| 局部变量和参数跳转到其声明                | 支持   |                                                             |
| 穿透宏包装跳转到底层声明                  | 支持   |                                                             |
| 由宏体或 Token 粘贴产生的名称锚定在调用点 | 支持   |                                                             |
| `#define` 体内的 Token 本身不提供导航     | 支持   |                                                             |
| 错误恢复                                  | 不支持 |                                                             |
| 未实例化模板中的依赖成员导航              | 支持   |                                                             |
| 模板特化导航到主模板                      | 不支持 | [clangd#212](https://github.com/clangd/clangd/issues/212)   |
| `auto` 关键字导航到推导得到的类型         | 不支持 | [clangd#2055](https://github.com/clangd/clangd/issues/2055) |

### 跨 TU 跳转到定义

一个翻译单元中的引用可解析到另一个源文件提供的定义——结果来自整个项目范围，而不局限于当前文件。

`main.cpp`：

```cpp
#include "shared.h"

int run(int value) {
    return transform(value);
}
```

`lib.cpp`：

```cpp
#include "shared.h"

int transform(int value) {
    return value * 2;
}
```

`shared.h`：

```cpp
#pragma once

int transform(int value);
```

### 定义与声明在光标处交替切换

在引用处执行跳转到定义会到达定义。在定义处执行时，它会跳转到声明；在声明处执行时，它会跳转到定义——两个位置交替切换。对于以内联方式定义且没有单独声明的符号，结果仍为其定义。

```cpp
int scale(int value);

int scale(int value) {
    return value * 2;
}

int apply(int value) {
    return scale(value);
}
```

### 仅有声明的符号跳转到其声明

只有声明的符号——纯虚函数、`extern` 变量、类内静态常量——会解析到该声明，而不是不返回任何结果。

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

### 对 `#include` 指令执行跳转到定义

在 `#include` 行上执行跳转到定义会打开所包含的文件。无论是文件开头被编译进 Preamble（PCH）的包含指令，还是文件后面的普通包含指令，都支持这一操作。

`main.cpp`：

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

`extra.h`：

```cpp
inline int spacing() {
    return 2;
}
```

`panel.h`：

```cpp
#pragma once

int dimension();
```

### 局部变量和参数跳转到其声明

对局部变量或参数执行跳转到定义，会跳转到它在函数体内的声明。

```cpp
int accumulate(int base) {
    int total = base;
    total = total + base;
    return total;
}
```

### 穿透宏包装跳转到底层声明

宏实参中写出的名称锚定在其拼写位置，因此定义与声明会在该处像在普通位置一样交替跳转；后续对该名称的使用会穿透宏包装，解析到它所声明的函数。

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

### 由宏体或 Token 粘贴产生的名称锚定在调用点

通过 Token 粘贴组装的名称在源代码中没有自己的拼写，因此它锚定在创建它的宏调用处：该调用就是它的定义位置，而对该名称的普通使用会跳回该调用点。

```cpp
#define MAKE_FLAG(name) bool flag_##name = false

MAKE_FLAG(verbose);

bool read_flag() {
    return flag_verbose;
}
```

### `#define` 体内的 Token 本身不提供导航

宏体中的 Token 在宏展开为其赋予含义之前没有任何意义，因此对其执行代码导航不会得到任何结果；而调用处的 Token 始终解析到被展开的宏。

```cpp
#define DEFINE_COUNTER int counter = 0

DEFINE_COUNTER;
```

### 错误恢复

导航到类型未解析的变量

当变量的类型名无法解析时，对该变量后续用法执行转到定义目前不会得到任何结果，即使该变量自身的声明仍有记录。

```cpp
Unresolved handle;  // 'Unresolved' does not name a type

void read() {
    (void) handle;  // go-to-def on handle → the declaration above
}
```

### 未实例化模板中的依赖成员导航

在一个从未实例化的模板中，访问依赖类型对象的成员时，会解析到相应类模板中声明的成员。

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

### 模板特化导航到主模板

对显式特化的名称执行转到定义时，会解析到该特化本身；目前不支持从该特化继续跳转到它所特化的主模板。

```cpp
template <typename T>
struct Formatter {}; // primary template

template <>
struct Formatter<int> {}; // go-to-def on Formatter → primary template
```

### `auto` 关键字导航到推导得到的类型

对 `auto` 关键字执行转到定义时，应跳转到由它推导得到的类型；目前不会得到任何结果。

```cpp
struct Widget {};

Widget make_widget();

void use() {
    auto widget = make_widget(); // go-to-def on auto → Widget
}
```

<!-- END GENERATED ITEMS -->

### 隐式代码导航

导航到隐式调用的代码定义。在 C++ 中，许多语法结构会产生对构造函数、运算符、转换等的隐藏调用。从相应语法结构（花括号、关键字或运算符 Token）导航到实际调用的函数，对于理解真正执行的代码至关重要。

隐式导航要求源 Token 没有歧义——如果某种模式中的 Token 已有明确的转到定义目标（例如变量名始终指向其声明），就不能将其改用于隐式调用导航。

<!-- BEGIN GENERATED ITEMS: implicit_code_navigation -->

| 能力                                  | 状态     | 问题                                                        |
| ------------------------------------- | -------- | ----------------------------------------------------------- |
| `override` / `final`                  | 不支持   |                                                             |
| `break` / `continue`                  | 不支持   | [clangd#1921](https://github.com/clangd/clangd/issues/1921) |
| 构造函数调用                          | 支持     |                                                             |
| 拷贝/移动构造与赋值                   | 部分支持 |                                                             |
| CTAD                                  | 支持     |                                                             |
| 聚合初始化                            | 支持     |                                                             |
| `delete` 表达式                       | 不支持   |                                                             |
| `new` 表达式                          | 部分支持 |                                                             |
| 成员初始化列表                        | 部分支持 |                                                             |
| 委托构造函数                          | 部分支持 |                                                             |
| 继承构造函数                          | 部分支持 |                                                             |
| 返回值隐式构造                        | 支持     |                                                             |
| Lambda 初始化捕获                     | 不支持   |                                                             |
| 重载运算符                            | 支持     |                                                             |
| C++20 重写运算符                      | 支持     |                                                             |
| 用户定义字面量                        | 不支持   |                                                             |
| 隐式转换运算符                        | 不支持   | [clangd#1931](https://github.com/clangd/clangd/issues/1931) |
| 调用构造函数或转换运算符的类型转换    | 部分支持 |                                                             |
| 范围 for 循环（range-based for）      | 不支持   |                                                             |
| 结构化绑定（structured bindings）     | 不支持   |                                                             |
| `co_await` / `co_yield` / `co_return` | 部分支持 |                                                             |

### `override` / `final`

导航到被重写的基类方法

在 `override` 或 `final` 说明符上执行转到定义，应跳转到它所重写的基类虚函数；目前不会返回任何结果。

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

导航到所在循环或 switch 语句的头部

在 `break` 或 `continue` 上执行转到定义，应跳转到其所在循环或 switch 语句的头部；目前不会返回任何结果。

```cpp
void loop() {
    for (int i = 0; i < 10; i += 1) {
        if (i == 5) break;  // go-to-def on break → the for loop
        continue;           // go-to-def on continue → the for loop
    }
}
```

### 构造函数调用

从圆括号或花括号导航到选中的构造函数

在构造函数调用的左圆括号或左花括号上执行转到定义，会跳转到重载决议选中的构造函数；`T(args)` 和 `T{args}` 两种形式均适用。

```cpp
struct Widget {
    Widget(int w, int h);
};

void build() {
    Widget a(800, 600);
    Widget b{800, 600};
}
```

### 拷贝/移动构造与赋值

导航到构造函数或赋值运算符

在赋值表达式的 `=` 上执行转到定义，会跳转到赋值运算符。引入拷贝初始化或移动初始化的 `=`（`T b = a;`）属于初始化语法，而非运算符调用，目前尚无法解析。

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

导航到选中的构造函数

当类模板实参推导选中某个特化时，在构造函数调用上执行转到定义会跳转到选中的构造函数，而不仅仅是类模板。

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

### 聚合初始化

导航到结构体定义

聚合体没有构造函数，因此在其初始化器的左花括号上执行转到定义会跳转到该聚合体的定义。

```cpp
struct Point {
    int x;
    int y;
};

void use() {
    auto p = Point{1, 2};
}
```

### `delete` 表达式

导航到析构函数

在 `delete` 上执行转到定义，应跳转到它所调用的析构函数；目前不会返回任何结果。

```cpp
struct Widget {
    ~Widget();
};

void dispose(Widget* widget) {
    delete widget;  // go-to-def on delete → Widget::~Widget
}
```

### `new` 表达式

导航到构造函数和重载的 `operator new`

在 `new` 上执行转到定义会跳转到类中重载的 `operator new`。返回结果不包含同一表达式所调用的构造函数。

```cpp
struct Pool {
    Pool();
    static void* operator new(decltype(sizeof(0)) size);
};

void make() {
    Pool* p = new Pool();
}
```

### 成员初始化列表

导航到基类和成员的构造函数

可从每个初始化器的左圆括号导航到初始化列表所调用的基类和成员构造函数。初始化器名称本身会解析为基类类型或成员，因此需要通过圆括号导航到构造函数。

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

### 委托构造函数

导航到目标构造函数

可从委托调用的左圆括号导航到委托构造函数的目标。构造函数名本身会解析为类类型，因此需要通过圆括号导航到目标构造函数。

```cpp
struct Widget {
    Widget(int w, int h);
    Widget() : Widget(0, 0) {}
};
```

### 继承构造函数

导航到通过 `using` 引入的基类构造函数

在继承构造函数声明（`using Base::Base;`）上执行转到定义，会跳转到一个基类构造函数。当基类声明了多个构造函数时，返回结果只会解析到其中一个，而不会列出整个集合。

```cpp
struct Base {
    Base(int x);
    Base(int x, int y);
};

struct Derived : Base {
    using Base::Base;
};
```

### 返回值隐式构造

导航到构造函数

花括号形式的 `return {args}` 会隐式构造该函数的返回类型；在花括号上执行转到定义会跳转到选中的构造函数。

```cpp
struct Widget {
    Widget(int w, int h);
};

Widget create() {
    return {800, 600};
}
```

### Lambda 初始化捕获

导航到构造函数

在 Lambda 初始化捕获的 `=` 上执行转到定义，应跳转到负责构造被捕获值的构造函数；目前不会返回任何结果。

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

### 重载运算符

从运算符 Token 导航到其定义

在重载运算符的 Token 上执行跳转到定义，会到达该运算符的定义处。
二元运算符、下标运算符、调用运算符和箭头运算符（`+`、`[]`、`()`、`->`）均可解析。

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

### C++20 重写运算符

跳转到重写实际使用的运算符

对于由 C++20 重写规则合成的比较，在源码中写出的运算符上执行跳转到定义，会到达实际实现该比较的运算符：`!=` 会跳转到 `operator==`，`>` 会跳转到 `operator<=>`。

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

### 用户定义字面量

跳转到字面量运算符

在用户定义字面量的后缀上执行跳转到定义，应该到达对应的 `operator""`；目前不会返回任何结果。

```cpp
struct Duration {
    unsigned long long ticks;
};

Duration operator""_ms(unsigned long long value);

void use() {
    Duration d = 500_ms;  // go-to-def on _ms → operator""_ms
}
```

### 隐式转换运算符

从转换上下文跳转到运算符

从调用用户定义转换的上下文（条件表达式、`!`、显式 `bool(...)`）执行跳转到定义，应该到达转换运算符；目前不会返回任何结果。

```cpp
struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if (g) {}      // go-to-def on ( → Guard::operator bool
    bool ok = !g;  // go-to-def on ! → Guard::operator bool
}
```

### 调用构造函数或转换运算符的类型转换

对用于构造目标对象的 `static_cast` 执行跳转到定义，会到达所选的构造函数。调用用户定义转换运算符的 `static_cast` 尚不能跳转到该运算符。

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

### 范围 for 循环（range-based for）

跳转到 `begin()` / `end()`

对范围 for 循环的 `:` 执行跳转到定义，应该到达为该范围选择的 `begin()` / `end()`；目前不会返回任何结果。

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

### 结构化绑定（structured bindings）

跳转到底层访问器或字段

对结构化绑定名称执行跳转到定义，会解析到绑定本身，而不是该名称对应的底层字段或访问器。

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

跳转到 awaiter 或 promise 方法

对 `co_yield` 执行跳转到定义，会到达 promise 的 `yield_value`。`co_await` 和 `co_return` 关键字尚不能跳转到 awaiter 或 promise 的方法。

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

## 跳转到声明

从符号的使用处或定义处跳转到其声明。C++ 中的许多实体具有独立的声明和定义。

clice 会返回所有声明位置和定义位置（直接在声明处定义的符号没有单独的声明位置），并排除光标当前所在的位置，因此可以在声明位置与定义位置之间切换，其行为与跳转到定义相同。

<!-- BEGIN GENERATED ITEMS: go_to_declaration -->

| 能力                         | 状态 | 问题 |
| ---------------------------- | ---- | ---- |
| 跨 TU 跳转到声明             | 支持 |      |
| 函数                         | 支持 |      |
| 类和结构体的前向声明         | 支持 |      |
| 静态数据成员                 | 支持 |      |
| `extern` 变量                | 支持 |      |
| 多重声明                     | 支持 |      |
| 签名仅有形式差异的声明与定义 | 支持 |      |

### 跨 TU 跳转到声明

从使用处执行跳转到声明，可以解析出其他文件中的位置：函数原型位于共享头文件中，行外定义位于同级源文件中；从另一个文件中的使用处跳转时，这两个位置都会列出。

`main.cpp`：

```cpp
#include "shared.h"

int run(int value) {
    return scale(value);
}
```

`lib.cpp`：

```cpp
#include "shared.h"

int scale(int value) {
    return value * 2;
}
```

`shared.h`：

```cpp
#pragma once

int scale(int value);
```

### 函数

从使用处或行外定义跳转到原型

无论从调用点还是行外定义执行跳转到声明，都能到达函数原型——原型会与另外两个非光标位置交替成为目标。

```cpp
struct Widget {
    void draw();
};

void Widget::draw() {}

void render(Widget& widget) {
    widget.draw();
}
```

### 类和结构体的前向声明

对于先有前向声明、后有定义的类，从使用处执行跳转到声明时，会同时列出两者；前向声明仍属于声明集合，不会因已有定义而被丢弃。

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

### 静态数据成员

跳转到类内声明

静态数据成员在类内声明、在类外定义；从使用处执行跳转到声明，会同时列出类内声明和定义。

```cpp
struct Config {
    static int timeout;
};

int Config::timeout = 30;

int read_config() {
    return Config::timeout;
}
```

### `extern` 变量

跳转到声明

使用 `extern` 变量时，会同时列出 `extern` 声明和起定义作用的声明，因此始终可以从使用处跳转到头文件中的声明。

```cpp
extern int log_level;

int log_level = 0;

int read_level() {
    return log_level;
}
```

### 多重声明

每个声明位置

当实体在多个位置声明时，从使用处执行跳转到声明，会列出所有声明位置，而不只是最近的位置。

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

### 签名仅有形式差异的声明与定义

参数名以及参数上的顶层 `const` 都不属于函数类型：下面的声明和定义以不同形式书写了同一个函数，但“跳转到声明”仍能将使用处关联到函数原型。

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

## 跳转到实现

<!-- BEGIN GENERATED ITEMS: go_to_implementation -->

| 能力             | 状态   | 问题                                                      |
| ---------------- | ------ | --------------------------------------------------------- |
| 重写链           | 支持   |                                                           |
| 同级重写         | 支持   |                                                           |
| 非虚函数         | 不支持 | [clangd#854](https://github.com/clangd/clangd/issues/854) |
| 基类             | 支持   |                                                           |
| 模板鸭子类型导航 | 不支持 |                                                           |

### 重写链

重写链的每一层都跳转到各自的直接重写方法

在三级重写链中，对每个方法执行“跳转到实现”都会到达下一层的重写方法——从基类到中间类，从中间类到叶子类。

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

### 同级重写

所有同级重写方法

对虚方法执行“跳转到实现”会列出各同级派生类中的所有重写方法。

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

### 非虚函数

从声明到行外定义

对非虚函数声明执行“跳转到实现”应到达其行外定义，其结果应是“跳转到定义”结果的超集；目前不会返回任何结果。

```cpp
struct Widget {
    void draw();  // go-to-impl on draw → out-of-line definition below
};

void Widget::draw() {}
```

### 基类

所有派生类

对基类名执行“跳转到实现”会列出由它派生的类。

```cpp
struct Base {};

struct Circle : Base {};

struct Square : Base {};
```

### 模板鸭子类型导航

从依赖成员调用执行“跳转到实现”应列出所有已知实例化中的具体方法；泛型 Lambda 的依赖调用同样如此。目前不会返回任何结果。

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

## 跳转到类型定义

跳转到符号的类型定义。适用于变量、参数、字段以及任何其他有类型的命名实体。若类型是类型别名或类指针包装类型，跳转时应解包到其底层类型或所指类型。

<!-- BEGIN GENERATED ITEMS: go_to_type_definition -->

| 能力                     | 状态     | 问题                                                        |
| ------------------------ | -------- | ----------------------------------------------------------- |
| 变量和参数               | 支持     |                                                             |
| 类和结构体的字段         | 支持     |                                                             |
| `auto` 推导的变量        | 不支持   |                                                             |
| 从智能指针跳转到所指类型 | 部分支持 | [clangd#1026](https://github.com/clangd/clangd/issues/1026) |
| 类型别名                 | 部分支持 |                                                             |
| 结构化绑定变量           | 支持     |                                                             |

### 变量和参数

对局部变量或参数执行“跳转到类型定义”可到达其类型的定义。

```cpp
struct Widget {};

Widget make_widget();

int probe(Widget param) {
    Widget local = make_widget();
    return 0;
}
```

### 类和结构体的字段

对字段访问执行“跳转到类型定义”可到达字段类型的定义。

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

### `auto` 推导的变量

对 `auto` 推导的变量执行“跳转到类型定义”应到达推导所得类型的定义；目前该变量没有类型关联，因此不会返回任何结果。

```cpp
struct Widget {};

Widget make_widget();

void probe() {
    auto widget = make_widget();  // go-to-type-def on widget → Widget
}
```

### 从智能指针跳转到所指类型

对智能指针变量执行“跳转到类型定义”会到达包装类型本身；尚不支持解包并跳转到所指类型。

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

### 类型别名

对别名类型的变量执行“跳转到类型定义”会到达 `using` 或 `typedef` 声明；目前尚不会解开别名并跳转到底层类型的定义。

```cpp
struct Impl {};

using Handle = Impl;

typedef Impl LegacyHandle;

int use(Handle handle, LegacyHandle legacy) {
    return 0;
}
```

### 结构化绑定变量

对结构化绑定执行“跳转到类型定义”会到达所绑定成员类型的定义。

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

## 查找引用

<!-- BEGIN GENERATED ITEMS: find_references -->

| 能力                                               | 状态   | 问题                                                                                                                   |
| -------------------------------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------- |
| 跨 TU 查找引用                                     | 支持   |                                                                                                                        |
| 引用结果中包含声明和定义位置                       | 支持   |                                                                                                                        |
| 范围 for 循环（range-based for loop）的隐式引用    | 不支持 | [clangd#1081](https://github.com/clangd/clangd/issues/1081)                                                            |
| 隐式构造函数与析构函数调用                         | 不支持 |                                                                                                                        |
| 通过转发函数产生的引用                             | 不支持 | [clangd#716](https://github.com/clangd/clangd/issues/716), [clangd#1872](https://github.com/clangd/clangd/issues/1872) |
| 依赖上下文和模板上下文中的引用                     | 不支持 | [clangd#258](https://github.com/clangd/clangd/issues/258), [clangd#675](https://github.com/clangd/clangd/issues/675)   |
| 引用的读/写分类                                    | 不支持 | [clangd#2139](https://github.com/clangd/clangd/issues/2139)                                                            |
| 随每条引用显示其所在函数                           | 不支持 | [clangd#177](https://github.com/clangd/clangd/issues/177)                                                              |
| 宏展开、`#ifdef`、`#ifndef` 与 `#undef` 中的宏引用 | 支持   |                                                                                                                        |
| 写在其他宏定义体内的宏引用                         | 不支持 | [clangd#346](https://github.com/clangd/clangd/issues/346)                                                              |
| 标签与 goto 引用                                   | 支持   |                                                                                                                        |

### 跨 TU 查找引用

查找引用也会收集其他文件中的使用位置：如果一个函数在一个源文件中定义，并在同级的另一个源文件中被调用，结果会同时报告两个调用点以及共享头文件中的声明，而不只是当前文件中的使用位置。

`main.cpp`：

```cpp
#include "shared.h"

int run(int value) {
    return compute(value);
}
```

`lib.cpp`：

```cpp
#include "shared.h"

int compute(int value) {
    return value * 2;
}

int again(int value) {
    return compute(value) + 1;
}
```

`shared.h`：

```cpp
#pragma once

int compute(int value);
```

### 引用结果中包含声明和定义位置

引用查询会返回声明、类外定义和每个使用位置，因此从符号的任意一处都能访问它的所有出现位置。

```cpp
int scale(int value);

int scale(int value) {
    return value * 2;
}

int use() {
    return scale(3);
}
```

### 范围 for 循环（range-based for loop）的隐式引用

对 `begin` 执行查找引用只会报告它自身的声明；隐式调用它的范围 for 循环不会包含在引用结果中。

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

### 隐式构造函数与析构函数调用

对构造函数执行查找引用只会报告它显式出现的位置；隐式调用该构造函数或其析构函数的对象定义不会包含在结果中。

```cpp
struct Blob {
    Blob();  // find-refs here omits the `Blob b;` definition below
    ~Blob();
};

void use() {
    Blob b;
}
```

### 通过转发函数产生的引用

查找构造函数的引用时，结果不包含经由完美转发工厂间接调用它的调用点。

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

### 依赖上下文和模板上下文中的引用

查找成员的引用时，结果不包含模板中的依赖调用点，即使使用该成员所属的类实例化了模板。

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

### 引用的读/写分类

引用响应只包含位置信息，因此无法区分读取与写入；也不支持为每项结果标注其访问类型。

```cpp
int use() {
    int x = 0;      // write
    int y = x + 1;  // read
    x = y;          // write
    return x;
}
```

### 随每条引用显示其所在函数

每条引用都只报告一个位置；不会附带该引用所在函数的名称，因此结果除文件和行号外不包含其他上下文。

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

### 宏展开、`#ifdef`、`#ifndef` 与 `#undef` 中的宏引用

宏的引用包括它的展开、用于测试它的 `#ifdef` / `#ifndef` 条件指令，以及撤销其定义的 `#undef`。同一名称的每个 `#define` 都对应一个独立符号，因此在 `#undef` 后重新定义的宏只会收集属于该次定义的使用位置。

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

### 写在其他宏定义体内的宏引用

查找宏的引用时，不会包括其他宏定义体内对该宏的提及。

```cpp
#define WIDTH 100  // find-refs here omits the WIDTH tokens in AREA below

#define AREA (WIDTH * WIDTH)

int total = AREA;
```

### 标签与 goto 引用

查找标签的引用时，会列出标签本身以及所有跳转到该标签的 `goto`。

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

## 调用层次结构

<!-- BEGIN GENERATED ITEMS: call_hierarchy -->

| 能力                         | 状态     | 问题                                                        |
| ---------------------------- | -------- | ----------------------------------------------------------- |
| 为函数和方法准备调用层次结构 | 支持     |                                                             |
| 传入调用                     | 支持     |                                                             |
| 传出调用                     | 支持     |                                                             |
| 条目 detail 字段中的函数签名 | 不支持   |                                                             |
| 成员函数的限定名             | 部分支持 |                                                             |
| 跟踪虚函数派发               | 不支持   |                                                             |
| 非函数目标                   | 不支持   | [clangd#1308](https://github.com/clangd/clangd/issues/1308) |
| Lambda 内部的调用            | 支持     |                                                             |
| 经由转发函数的构造函数调用   | 不支持   | [clangd#2242](https://github.com/clangd/clangd/issues/2242) |

### 为函数和方法准备调用层次结构

无论是非成员函数还是成员函数，都可以准备调用层次结构，并以光标下的实体为起点创建条目。

```cpp
struct Service {
    void start();
};

void Service::start() {}

void launch(Service& s) {
    s.start();
}
```

### 传入调用

传入调用会列出函数的所有调用者；若同一个调用者多次调用该函数，则每个调用点都会计入。

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

### 传出调用

传出调用会列出函数体调用的所有函数，每个被调用者对应一个条目。

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

### 条目 detail 字段中的函数签名

调用层次结构条目只包含名称；detail 字段中不会附带函数签名，因此无法在层次结构中区分重载。

```cpp
int compute(int a, int b) {  // no signature attached to this item
    return a + b;
}

int caller() {
    return compute(1, 2);
}
```

### 成员函数的限定名

可以生成成员函数的调用层次结构条目，但其 name 字段只包含未限定的方法名（`draw`），而不是能将它与非成员函数区分开的限定名 `Circle::draw`。

```cpp
struct Circle {
    void draw();
};

void Circle::draw() {}
```

### 跟踪虚函数派发

基类虚方法的传入调用不包括对派生类重写方法的调用；对重写方法的调用只归属于该重写方法，绝不会归属于它所重写的基类方法。

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

### 非函数目标

变量和枚举常量

在变量或枚举常量上准备调用层次结构不会返回任何结果；该请求仅适用于函数和方法。

```cpp
int counter = 0;  // prepare call hierarchy here → nothing

enum Mode {
    Fast,  // prepare call hierarchy here → nothing
    Slow,
};
```

### Lambda 内部的调用

写在 Lambda 函数体中的调用会出现在其所调用函数的传入调用中，并归属于包围该 Lambda 的函数。

```cpp
void foo() {}

void use() {
    auto task = [] {
        foo();
    };
    task();
}
```

### 经由转发函数的构造函数调用

构造函数的传入调用不包括经由完美转发工厂函数调用该构造函数的调用点。

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

## 类型层次结构

<!-- BEGIN GENERATED ITEMS: type_hierarchy -->

| 能力                                             | 状态     | 问题                                                    |
| ------------------------------------------------ | -------- | ------------------------------------------------------- |
| 为 class、struct、enum 和 union 准备类型层次结构 | 支持     |                                                         |
| 父类型                                           | 支持     |                                                         |
| 子类型                                           | 支持     |                                                         |
| 模板继承                                         | 支持     |                                                         |
| 类型层次结构条目中的模板参数                     | 部分支持 | [clangd#31](https://github.com/clangd/clangd/issues/31) |

### 为 class、struct、enum 和 union 准备类型层次结构

准备类型层次结构时，可以将条目锚定到任何用户定义类型标签上——class、struct、enum 和 union 均可。

```cpp
class Handle {};

struct Point {};

enum class Mode {};

union Storage {
    int i;
    float f;
};
```

### 父类型

父类型会列出类的所有直接基类，包括多重继承派生类型的每一个基类。

```cpp
struct Alpha {};

struct Beta {};

struct Gamma : Alpha, Beta {};
```

### 子类型

子类型会列出从某个基类派生的所有类，涵盖各个同级派生类型。

```cpp
struct Shape {};

struct Circle : Shape {};

struct Square : Shape {};

struct Triangle : Shape {};
```

### 模板继承

基类的子类型包括通过类模板从其派生的类，例如 CRTP 包装器。

```cpp
struct Base {};

template <typename T>
struct CRTP : Base {};

struct Widget : CRTP<Widget> {};
```

### 类型层次结构条目中的模板参数

由类模板特化产生的子类型会被列出，但其条目名称仅为不带实参的模板名（`Derived`），不含能够区分 `Derived<Foo>` 的模板参数。

```cpp
struct Foo {};

struct Base {};

template <typename T>
struct Derived : Base {};

Derived<Foo> instance;
```

<!-- END GENERATED ITEMS -->

## 工作区符号

按名称在整个项目中搜索符号（`workspace/symbol`）。

<!-- BEGIN GENERATED ITEMS: workspace_symbol -->

| 能力                             | 状态     | 问题                                                        |
| -------------------------------- | -------- | ----------------------------------------------------------- |
| 基本的工作区符号搜索             | 支持     |                                                             |
| 搜索覆盖整个项目                 | 支持     |                                                             |
| 重载区分                         | 部分支持 | [clangd#1344](https://github.com/clangd/clangd/issues/1344) |
| 模糊匹配                         | 不支持   | [clangd#914](https://github.com/clangd/clangd/issues/914)   |
| 部分限定名搜索                   | 不支持   | [clangd#550](https://github.com/clangd/clangd/issues/550)   |
| 在枚举作用域内查找枚举项         | 不支持   | [clangd#931](https://github.com/clangd/clangd/issues/931)   |
| 别名所指向的声明排在类型别名前面 | 不支持   | [clangd#2253](https://github.com/clangd/clangd/issues/2253) |
| 按修饰名（链接器名称）搜索       | 不支持   |                                                             |

### 基本的工作区符号搜索

不区分大小写的子串匹配

查询会匹配所有名称中包含查询文本的符号，且不区分大小写：函数、类型、枚举项和宏都会参与匹配；如果没有匹配项，则返回空列表而不是错误。

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

### 搜索覆盖整个项目

来自查询文件之外其他文件的匹配结果

查询会返回甚至尚未在编辑器中打开的项目文件中的符号：这里的 `other.h` 仍处于关闭状态，因此其中的匹配结果由后台索引提供。

`main.cpp`：

```cpp
// query: helper_elsewhere

int local_anchor = 0;
```

`other.h`：

```cpp
void helper_elsewhere() {}
```

### 重载区分

结果中显示参数类型

查询重载名称会找到所有重载，但每个条目都只有裸名称——只有打开两个位置，才能区分两个 `process` 结果。

```cpp
// query: process

void process(int value) {}

void process(bool flag, int level) {}
```

### 模糊匹配

针对 camelCase 和 snake_case 的词边界感知评分

匹配采用不区分大小写的子串测试：`LinLis` 找不到 `LinkedList`，`pcfg` 也找不到 `parse_config`。词边界处的首字母应能匹配并参与评分，且适用于所有符号种类（包括宏）。

```cpp
// query: LinLis
// query: pcfg

struct LinkedList {};

void parse_config();
```

### 部分限定名搜索

符号只按裸名称匹配：即使 `deep::net::Socket` 存在，`net::Socket` 也找不到任何结果，其他带限定符前缀的形式同样如此。

```cpp
// query: net::Socket

namespace deep {
namespace net {

struct Socket {};

}  // namespace net
}  // namespace deep
```

### 在枚举作用域内查找枚举项

`Color::Red` 应该能找到枚举项——无论是有作用域枚举还是无作用域枚举——但限定查询无法匹配任何结果；只有裸名称 `Red` 可以。

```cpp
// query: Color::Red

enum Color { Red, Green };
```

### 别名所指向的声明排在类型别名前面

当 `ConnectionImpl` 和指向它的别名 `Connection` 同时匹配某项查询时，被别名指向的声明应排在前面。目前的结果不包含排序信息。

```cpp
// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
```

### 按修饰名（链接器名称）搜索

粘贴 `_Z7processi` 这样的链接器符号，应解析到该修饰名所对应的函数——这在追查链接器错误和堆栈跟踪时很有用。

```cpp
// query: _Z7processi

void process(int value);
```

<!-- END GENERATED ITEMS -->

## 模块导航

<!-- BEGIN GENERATED ITEMS: module_navigation -->

| 能力                                    | 状态     | 问题                                                        |
| --------------------------------------- | -------- | ----------------------------------------------------------- |
| `import module_name` 导航到模块接口单元 | 支持     | [clangd#2310](https://github.com/clangd/clangd/issues/2310) |
| `import :partition` 导航到分区单元      | 支持     |                                                             |
| 在同一模块的接口单元与实现单元之间导航  | 部分支持 |                                                             |
| 点分隔的模块名                          | 部分支持 |                                                             |

### `import module_name` 导航到模块接口单元

对 `import` 声明中的名称执行“转到定义”，会打开导出该模块的模块接口单元；使用已导入符号的位置则会跳转到该符号在该单元中的定义。

`main.cpp`：

```cpp
import widget;

int build() {
    return area(2, 3);
}
```

`widget.cppm`：

```cpp
export module widget;

export int area(int width, int height) {
    return width * height;
}
```

### `import :partition` 导航到分区单元

对分区导入中冒号后的分区名执行“转到定义”，会打开声明该分区的分区单元。

`main.cpp`：

```cpp
import pack;

int run() {
    return count();
}
```

`pack.cppm`：

```cpp
export module pack;

export import :items;
```

`pack_items.cppm`：

```cpp
export module pack:items;

export int count() {
    return 3;
}
```

### 在同一模块的接口单元与实现单元之间导航

对实现单元（`module m;`）中的模块名执行“转到定义”，会跳转到声明该模块的接口单元；但不提供反向导航，即无法从接口名称导航到实现。

`main.cpp`：

```cpp
import store;

int lookup(int key) {
    return fetch(key);
}
```

`iface.cppm`：

```cpp
export module store;

export int fetch(int key);
```

`impl.cpp`：

```cpp
module store;

int fetch(int key) {
    return key * 2;
}
```

### 点分隔的模块名

导航每个名称段

对点分隔模块名的首个名称段执行“转到定义”，可到达该模块的接口单元；点号后的名称段目前还无法单独解析。

`main.cpp`：

```cpp
import app.core;

int run() {
    return value();
}
```

`app_core.cppm`：

```cpp
export module app.core;

export int value() {
    return 1;
}
```

<!-- END GENERATED ITEMS -->

## 文档高亮

高亮当前文件中光标所在符号的所有引用（`textDocument/documentHighlight`）。

<!-- BEGIN GENERATED ITEMS: document_highlight -->

| 能力                                 | 状态   | 问题                                                        |
| ------------------------------------ | ------ | ----------------------------------------------------------- |
| 高亮当前文件中光标所在符号的所有引用 | 不支持 |                                                             |
| 符号高亮的读/写分类                  | 不支持 |                                                             |
| 控制流 Token 高亮                    | 不支持 | [clangd#1921](https://github.com/clangd/clangd/issues/1921) |

### 高亮当前文件中光标所在符号的所有引用

将光标放在 `total` 上时，应高亮其声明及该文件中的每处使用；该请求尚未实现。

```cpp
int total = 0;

void accumulate(int amount) {
    total = total + amount;
}
```

### 符号高亮的读/写分类

每处高亮都应包含其访问类型，以便编辑器对写操作和读操作使用不同的着色。

```cpp
void tally() {
    int count = 0;      // write
    int next = count;   // read
    count = next;       // write
}
```

### 控制流 Token 高亮

高亮 `break` 或 `continue` 时，还应高亮其所属的循环或 `switch`；高亮 `return` / `throw` 时，则应高亮它们所标示的函数退出点。

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

## 源文件/头文件切换

<!-- BEGIN GENERATED ITEMS: switch_source_header -->

| 能力                       | 状态   | 问题 |
| -------------------------- | ------ | ---- |
| 在源文件与其头文件之间切换 | 不支持 |      |

### 在源文件与其头文件之间切换

在 `widget.cpp` 中，只需一条命令即可跳转到 `widget.h`，也可从后者跳转回来——clangd 客户端所依赖的 `textDocument/switchSourceHeader` 请求尚未实现。

```cpp
// widget.h
class Widget {
    void draw();
};

// widget.cpp — #include "widget.h"
void Widget::draw() {}
```

<!-- END GENERATED ITEMS -->
