# 代码导航

## 跳转到定义

<!-- BEGIN GENERATED ITEMS: go_to_definition -->

| 能力                                      | 状态   | 问题                                                        |
| ----------------------------------------- | ------ | ----------------------------------------------------------- |
| 跨翻译单元 go-to-definition               | 支持   |                                                             |
| 定义与声明在光标处交替切换                | 支持   |                                                             |
| 仅有声明的符号导航到其声明                | 支持   |                                                             |
| 在 `#include` 指令上使用 go-to-definition | 支持   |                                                             |
| 局部变量和参数导航到其声明                | 支持   |                                                             |
| 穿透宏包装导航到底层声明                  | 支持   |                                                             |
| 由宏体或 token 粘贴产生的名字锚定在调用点 | 支持   |                                                             |
| `#define` 体内的 token 不携带自身的导航   | 支持   |                                                             |
| 错误恢复                                  | 不支持 |                                                             |
| 未实例化模板中的依赖成员导航              | 支持   |                                                             |
| 模板特化导航到主模板                      | 不支持 | [clangd#212](https://github.com/clangd/clangd/issues/212)   |
| `auto` 关键字导航到推导出的类型           | 不支持 | [clangd#2055](https://github.com/clangd/clangd/issues/2055) |

### 跨翻译单元 go-to-definition

一个翻译单元中的使用解析到同级源文件提供的定义——答案覆盖整个项目，而不只是当前文件。

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

### 定义与声明在光标处交替切换

在用法上，go-to-definition 到达定义。在定义上调用时它会跳转到声明，在声明上调用时它跳转到定义——两个位置交替切换。内联定义且没有单独声明的符号，其定义仍作为答案。

```cpp
int scale(int value);

int scale(int value) {
    return value * 2;
}

int apply(int value) {
    return scale(value);
}
```

### 仅有声明的符号导航到其声明

只有声明的符号——纯虚函数、`extern` 变量、类内静态常量——解析到该声明，而不是返回空结果。

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

### 在 `#include` 指令上使用 go-to-definition

在 `#include` 行上调用 go-to-definition 会打开被包含的文件。这适用于编译进 preamble（PCH）的前置包含，也适用于文件后面普通的包含。

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

### 局部变量和参数导航到其声明

对局部变量或参数使用 go-to-definition 会跳转到它在函数体内的声明。

```cpp
int accumulate(int base) {
    int total = base;
    total = total + base;
    return total;
}
```

### 穿透宏包装导航到底层声明

在宏参数中拼写的名字锚定在其拼写位置，因此定义和声明在那里与普通位置一样交替切换；后续的使用会穿透包装解析到它所声明的函数。

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

### 由宏体或 token 粘贴产生的名字锚定在调用点

通过 token 粘贴组装的名字在源代码中没有自己的拼写，因此它锚定在创建它的宏调用处：该调用就是它的定义位置，而对名字的普通使用会跳回该调用点。

```cpp
#define MAKE_FLAG(name) bool flag_##name = false

MAKE_FLAG(verbose);

bool read_flag() {
    return flag_verbose;
}
```

### `#define` 体内的 token 不携带自身的导航

写在宏体中的 token 在展开赋予其含义之前没有意义，因此在其上导航不会产生结果；而调用位置的 token 总是解析到正在展开的宏。

```cpp
#define DEFINE_COUNTER int counter = 0

DEFINE_COUNTER;
```

### 错误恢复

导航到类型未解析的变量

当变量的类型名无法解析时，对该变量后续使用的 go-to-definition 目前返回空结果，即使变量自身的声明仍被记录。

```cpp
Unresolved handle;  // 'Unresolved' does not name a type

void read() {
    (void) handle;  // go-to-def on handle → the declaration above
}
```

### 未实例化模板中的依赖成员导航

在从未实例化的模板内部，对依赖类型对象的成员访问解析到对应类模板上声明的成员。

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

对显式特化名称的 go-to-definition 解析到特化本身；从它跳转到所特化的主模板的功能尚未提供。

```cpp
template <typename T>
struct Formatter {}; // primary template

template <>
struct Formatter<int> {}; // go-to-def on Formatter → primary template
```

### `auto` 关键字导航到推导出的类型

对 `auto` 关键字的 go-to-definition 应该到达它推导出的类型；目前返回空结果。

```cpp
struct Widget {};

Widget make_widget();

void use() {
    auto widget = make_widget(); // go-to-def on auto → Widget
}
```

<!-- END GENERATED ITEMS -->

### 隐式代码导航

导航到隐式调用的代码定义。在 C++ 中，许多构造会生成对构造函数、运算符、转换等的隐藏调用。从语法构造（花括号、关键字、运算符 token）导航到实际被调用的函数，对于理解真正执行的代码至关重要。

隐式导航要求有一个无歧义的源 token —— 那些已为 token 定义了明确 go-to-def 目标的模式（例如变量名始终指向其声明）不能改用于隐式调用导航。

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
| 调用构造函数或转换运算符的转换        | 部分支持 |                                                             |
| 范围 for 循环                         | 不支持   |                                                             |
| 结构化绑定                            | 不支持   |                                                             |
| `co_await` / `co_yield` / `co_return` | 部分支持 |                                                             |

### `override` / `final`

导航到被重写的基类方法

在 `override` 或 `final` 说明符上执行 go-to-definition 应到达它所重写的基类虚函数；目前返回空结果。

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

导航到所在循环或 switch 头部

在 `break` 或 `continue` 上执行 go-to-definition 应到达它控制的循环或 switch 头部；目前返回空结果。

```cpp
void loop() {
    for (int i = 0; i < 10; i += 1) {
        if (i == 5) break;  // go-to-def on break → the for loop
        continue;           // go-to-def on continue → the for loop
    }
}
```

### 构造函数调用

从括号或花括号导航到选中的构造函数

在构造函数调用的左括号或左花括号上执行 go-to-definition 会到达重载决议选中的构造函数，`T(args)` 和 `T{args}` 两种形式都适用。

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

在赋值的 `=` 上执行 go-to-definition 会到达赋值运算符。用于引入拷贝或移动初始化的 `=`（`T b = a;`）是初始化语法而不是运算符调用，目前尚未解析。

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

当类模板实参推导选出一个特化时，在构造函数调用上执行 go-to-definition 会到达被选中的构造函数，而不仅仅是类模板。

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

聚合体没有构造函数，因此在其初始化花括号上执行 go-to-definition 会到达该聚合体的定义。

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

在 `delete` 上执行 go-to-definition 应到达它运行的析构函数；目前返回空结果。

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

在 `new` 上执行 go-to-definition 会到达类的重载 `operator new`。同一表达式调用的构造函数不在回复中。

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

导航到基类和成员构造函数

初始化列表运行的基类和成员构造函数通过每个初始化器的左括号到达。初始化器的名称本身解析为基类类型或成员，因此到构造函数的导航经过括号。

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

委托构造函数的目标通过委托调用的左括号到达。构造函数名本身解析为类类型，因此到目标构造函数的导航经过括号。

```cpp
struct Widget {
    Widget(int w, int h);
    Widget() : Widget(0, 0) {}
};
```

### 继承构造函数

导航到通过 `using` 引入的基类构造函数

在继承构造函数声明（`using Base::Base;`）上执行 go-to-definition 会到达一个基类构造函数。当基类声明了多个构造函数时，回复解析为其中之一，而不是列出整个集合。

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

花括号形式的 `return {args}` 隐式构造函数的返回类型；在花括号上执行 go-to-definition 会到达选中的构造函数。

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

在 lambda init-capture 的 `=` 上执行 go-to-definition 应到达构造捕获值的构造函数；目前返回空结果。

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

从运算符 token 导航到其定义

在重载运算符 token 上执行 go-to-definition 会到达运算符的定义。
二元、下标、调用和箭头运算符（`+`、`[]`、`()`、`->`）均可解析。

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

导航到重写实际使用的运算符

对于由 C++20 重写规则合成的比较，在书写的运算符上执行 go-to-definition 会到达实际实现它的运算符：`!=` 到达 `operator==`，`>` 到达 `operator<=>`。

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

导航到字面量运算符

在用户定义字面量后缀上执行 go-to-definition 应该到达对应的 `operator""`；目前返回空结果。

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

从转换上下文导航到运算符

从执行用户定义转换的上下文（条件、`!`、显式 `bool(...)`）执行 go-to-definition 应该到达转换运算符；目前返回空结果。

```cpp
struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if (g) {}      // go-to-def on ( → Guard::operator bool
    bool ok = !g;  // go-to-def on ! → Guard::operator bool
}
```

### 调用构造函数或转换运算符的转换

构造其目标的 `static_cast` 会到达选中的构造函数。运行用户定义转换运算符的 `static_cast` 尚未到达该运算符。

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

### 范围 for 循环

导航到 `begin()` / `end()`

对 range-based for 的 `:` 执行 go-to-definition 应该到达为该范围选择的 `begin()` / `end()`；目前返回空结果。

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

### 结构化绑定

导航到底层访问器或字段

对 structured binding 名称执行 go-to-definition 会解析到绑定本身，而不是它命名的底层字段或访问器。

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

导航到 awaiter 或 promise 方法

对 `co_yield` 执行 go-to-definition 会到达 promise 的 `yield_value`。`co_await` 和 `co_return` 关键字尚未到达 awaiter 或 promise 的方法。

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

从符号的使用处或定义处导航到其声明。C++ 中许多实体具有独立的声明和定义。

clice 返回声明位置加上定义位置 — 内联定义的符号没有独立的声明 — 并排除光标当前所在的位置，因此声明与定义位置像 go-to-definition 一样交替。

<!-- BEGIN GENERATED ITEMS: go_to_declaration -->

| 能力                       | 状态 | 问题 |
| -------------------------- | ---- | ---- |
| 跨 TU go-to-declaration    | 支持 |      |
| 函数                       | 支持 |      |
| 类和结构体的前向声明       | 支持 |      |
| 静态数据成员               | 支持 |      |
| `extern` 变量              | 支持 |      |
| 多重声明                   | 支持 |      |
| 声明与定义仅有表面签名差异 | 支持 |      |

### 跨 TU go-to-declaration

对使用处执行 go-to-declaration 会解析到其他文件中的位置：原型位于共享头文件中，行外定义位于相邻源文件中，两者都会从另一文件的使用处一并提供。

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

### 函数

从使用处或行外定义导航到原型

从调用处和行外定义执行 go-to-declaration 都能到达函数原型 — 这是原型交替出现的两个非光标位置。

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

具有前向声明和后续定义的类，从使用处会同时提供两者 — 前向声明保留在声明集合中，而不是被定义替代。

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

到类内声明

静态数据成员在类内声明并在类外定义；对使用处执行 go-to-declaration 会同时提供类内声明和定义。

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

到声明

使用 `extern` 变量会同时提供 `extern` 声明和定义性声明，因此使用处始终可以到达头文件侧的声明。

```cpp
extern int log_level;

int log_level = 0;

int read_level() {
    return log_level;
}
```

### 多重声明

每个声明位置

当实体在多个位置声明时，对使用处执行 go-to-declaration 会列出所有声明位置，而不只是最近的那个。

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

### 声明与定义仅有表面签名差异

参数名和参数上的顶层 `const` 不属于函数类型的一部分：下面的声明和定义拼写不同，但 go-to-declaration 仍能将使用处连接到原型。

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
| 虚方法           | 支持   |                                                           |
| 虚方法           | 支持   |                                                           |
| 非虚函数         | 不支持 | [clangd#854](https://github.com/clangd/clangd/issues/854) |
| 基类             | 支持   |                                                           |
| 模板鸭子类型导航 | 不支持 |                                                           |

### 虚方法

链中每一层跳到其下一层重写

在三级重写链中，对每个方法执行 go-to-implementation 都会到达下一层的重写：基类到中间类，中间类到叶子类。

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

### 虚方法

所有同层重写

对虚方法执行 go-to-implementation 会列出所有同级派生类中的重写。

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

声明到行外定义

对非虚函数声明执行 go-to-implementation 应到达其行外定义，表现为 go-to-definition 的超集；目前返回空。

```cpp
struct Widget {
    void draw();  // go-to-impl on draw → out-of-line definition below
};

void Widget::draw() {}
```

### 基类

所有派生类

对基类名执行 go-to-implementation 会列出所有从它派生的类。

```cpp
struct Base {};

struct Circle : Base {};

struct Square : Base {};
```

### 模板鸭子类型导航

从依赖成员调用出发，go-to-implementation 应列出每个已知实例化的具体方法；对泛型 lambda 的依赖调用同样适用。目前返回空。

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

导航到符号的类型定义。适用于变量、参数、字段以及任何其他具有类型的命名实体。当类型是类型别名或类似指针的包装时，导航应解包到底层/指向的类型。

<!-- BEGIN GENERATED ITEMS: go_to_type_definition -->

| 能力               | 状态     | 问题                                                        |
| ------------------ | -------- | ----------------------------------------------------------- |
| 变量和参数         | 支持     |                                                             |
| 类与结构体字段     | 支持     |                                                             |
| `auto` 推导的变量  | 不支持   |                                                             |
| 智能指针到指向类型 | 部分支持 | [clangd#1026](https://github.com/clangd/clangd/issues/1026) |
| 类型别名           | 部分支持 |                                                             |
| 结构化绑定变量     | 支持     |                                                             |

### 变量和参数

对局部变量或参数执行 go-to-type-definition 会到达其类型的定义。

```cpp
struct Widget {};

Widget make_widget();

int probe(Widget param) {
    Widget local = make_widget();
    return 0;
}
```

### 类与结构体字段

对字段访问执行 go-to-type-definition 会到达其字段类型的定义。

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

对 `auto` 推导出的变量执行 go-to-type-definition 应到达推导类型的定义；目前该变量不带类型关系，因此返回空。

```cpp
struct Widget {};

Widget make_widget();

void probe() {
    auto widget = make_widget();  // go-to-type-def on widget → Widget
}
```

### 智能指针到指向类型

对智能指针变量执行 go-to-type-definition 会到达包装类型本身；不会提供到指向类型的解包。

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

对别名类型变量执行 go-to-type-definition 会到达 `using` 或 `typedef` 声明；目前还不会将别名解包到底层类型的定义。

```cpp
struct Impl {};

using Handle = Impl;

typedef Impl LegacyHandle;

int use(Handle handle, LegacyHandle legacy) {
    return 0;
}
```

### 结构化绑定变量

对结构化绑定执行 go-to-type-definition 会到达所绑定成员类型的定义。

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

| 能力                                           | 状态   | 问题                                                                                                                   |
| ---------------------------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------- |
| 跨翻译单元 find references                     | 支持   |                                                                                                                        |
| 声明和定义位置出现在引用中                     | 支持   |                                                                                                                        |
| range-based for 循环的隐式引用                 | 不支持 | [clangd#1081](https://github.com/clangd/clangd/issues/1081)                                                            |
| 隐式构造函数与析构函数调用                     | 不支持 |                                                                                                                        |
| 通过转发函数的引用                             | 不支持 | [clangd#716](https://github.com/clangd/clangd/issues/716), [clangd#1872](https://github.com/clangd/clangd/issues/1872) |
| 依赖与模板上下文中的引用                       | 不支持 | [clangd#258](https://github.com/clangd/clangd/issues/258), [clangd#675](https://github.com/clangd/clangd/issues/675)   |
| 引用的读/写分类                                | 不支持 | [clangd#2139](https://github.com/clangd/clangd/issues/2139)                                                            |
| 每条引用附带所在函数名                         | 不支持 | [clangd#177](https://github.com/clangd/clangd/issues/177)                                                              |
| 宏引用覆盖展开、`#ifdef`/`#ifndef` 和 `#undef` | 支持   |                                                                                                                        |
| 写在其他宏定义体内的宏引用                     | 不支持 | [clangd#346](https://github.com/clangd/clangd/issues/346)                                                              |
| Label 与 goto 引用                             | 支持   |                                                                                                                        |

### 跨翻译单元 find references

find references 也会收集其他文件中的使用：一个函数在一个源文件中定义并在另一个源文件中被调用时，会同时报告两个调用点以及共享头文件中的声明，而不只是当前文件中的使用。

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

### 声明和定义位置出现在引用中

引用查询会返回声明和行外定义以及每个使用位置，因此从符号的任意一处都能到达它的全部出现位置。

```cpp
int scale(int value);

int scale(int value) {
    return value * 2;
}

int use() {
    return scale(3);
}
```

### range-based for 循环的隐式引用

对 `begin` 执行 find references 只报告其自身声明；隐式调用它的
range-based for 循环不会出现在引用结果中。

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

对构造函数执行 find references 只报告显式调用位置；隐式调用构造函数或析构函数的对象定义不会被包含。

```cpp
struct Blob {
    Blob();  // find-refs here omits the `Blob b;` definition below
    ~Blob();
};

void use() {
    Blob b;
}
```

### 通过转发函数的引用

对构造函数执行 find references 不包含通过完美转发工厂间接到达它的调用位置。

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

### 依赖与模板上下文中的引用

对成员执行 find references 不包含模板中的依赖调用位置，即使模板以该成员所在类实例化。

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

引用回复只携带位置信息，读者无法区分写与读；未提供为每条结果标注访问类型的支持。

```cpp
int use() {
    int x = 0;      // write
    int y = x + 1;  // read
    x = y;          // write
    return x;
}
```

### 每条引用附带所在函数名

每条引用只报告为裸位置；未附加包含它的函数名，因此结果只有文件和行号，没有更多上下文。

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

### 宏引用覆盖展开、`#ifdef`/`#ifndef` 和 `#undef`

宏的引用涵盖其展开、测试它的 `#ifdef` / `#ifndef` 条件以及取消它的 `#undef`。每次 `#define` 同名符号都是独立的符号，因此 `#undef` 之后的重新定义只收集其自身的用例。

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

对宏执行 find references 不包含写在其他宏定义体内的提及该宏的地方。

```cpp
#define WIDTH 100  // find-refs here omits the WIDTH tokens in AREA below

#define AREA (WIDTH * WIDTH)

int total = AREA;
```

### Label 与 goto 引用

对 label 执行 find references 会列出该 label 本身以及所有跳转到它的 `goto`。

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

## 调用层次

<!-- BEGIN GENERATED ITEMS: call_hierarchy -->

| 能力                                    | 状态     | 问题                                                        |
| --------------------------------------- | -------- | ----------------------------------------------------------- |
| 对函数和方法执行 prepare call hierarchy | 支持     |                                                             |
| 入向调用                                | 支持     |                                                             |
| 出向调用                                | 支持     |                                                             |
| 条目的 detail 字段中显示函数签名        | 不支持   |                                                             |
| 成员函数的限定名                        | 部分支持 |                                                             |
| 跟踪虚函数派发                          | 不支持   |                                                             |
| 非函数目标                              | 不支持   | [clangd#1308](https://github.com/clangd/clangd/issues/1308) |
| lambda 内部的调用                       | 支持     |                                                             |
| 通过转发函数的构造函数调用              | 不支持   | [clangd#2242](https://github.com/clangd/clangd/issues/2242) |

### 对函数和方法执行 prepare call hierarchy

对自由函数和成员方法执行 prepare call hierarchy 均可正常工作，在光标所在的实体处建立条目。

```cpp
struct Service {
    void start();
};

void Service::start() {}

void launch(Service& s) {
    s.start();
}
```

### 入向调用

Incoming calls 列出函数的所有调用者，若某个调用者多次调用它，每个调用位置都会计入。

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

### 出向调用

Outgoing calls 列出函数体调用的每个函数，每个被调用者对应一个条目。

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

### 条目的 detail 字段中显示函数签名

Call hierarchy 条目只携带名称；函数签名未附加到 detail 字段，因此重载函数在层级中无法区分。

```cpp
int compute(int a, int b) {  // no signature attached to this item
    return a + b;
}

int caller() {
    return compute(1, 2);
}
```

### 成员函数的限定名

成员函数的 call hierarchy 条目已经生成，但其 name 字段只携带裸方法名（`draw`），而非能将其与自由函数区分的限定名 `Circle::draw`。

```cpp
struct Circle {
    void draw();
};

void Circle::draw() {}
```

### 跟踪虚函数派发

基类虚方法的 incoming calls 不包含通过派生类 override 进行的调用；对 override 的调用只归因于该 override，从不归因于其重写的基类方法。

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

在变量或枚举常量上准备 call hierarchy 不返回任何结果；该请求仅对函数和方法提供。

```cpp
int counter = 0;  // prepare call hierarchy here → nothing

enum Mode {
    Fast,  // prepare call hierarchy here → nothing
    Slow,
};
```

### lambda 内部的调用

写在 lambda 体内的调用会出现在它所调用函数的 incoming calls 中，并归属到包含该 lambda 的函数。

```cpp
void foo() {}

void use() {
    auto task = [] {
        foo();
    };
    task();
}
```

### 通过转发函数的构造函数调用

构造函数的 incoming calls 不包含通过完美转发工厂函数到达它的调用点。

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

## 类型层次

<!-- BEGIN GENERATED ITEMS: type_hierarchy -->

| 能力                                                  | 状态     | 问题                                                    |
| ----------------------------------------------------- | -------- | ------------------------------------------------------- |
| 在 class、struct、enum 和 union 上准备 type hierarchy | 支持     |                                                         |
| 父类型                                                | 支持     |                                                         |
| 子类型                                                | 支持     |                                                         |
| 模板继承                                              | 支持     |                                                         |
| type hierarchy 条目中的模板参数                       | 部分支持 | [clangd#31](https://github.com/clangd/clangd/issues/31) |

### 在 class、struct、enum 和 union 上准备 type hierarchy

准备 type hierarchy 会在任意用户定义类型标签（class、struct、enum、union 皆可）上锚定一个条目。

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

Supertypes 列出类的每个直接基类，包括多重继承派生类型的每个基类。

```cpp
struct Alpha {};

struct Beta {};

struct Gamma : Alpha, Beta {};
```

### 子类型

Subtypes 列出从基类派生的每个类，涵盖所有同级派生类型。

```cpp
struct Shape {};

struct Circle : Shape {};

struct Square : Shape {};

struct Triangle : Shape {};
```

### 模板继承

基类的 Subtypes 包括通过类模板派生自它的类，例如 CRTP 包装器。

```cpp
struct Base {};

template <typename T>
struct CRTP : Base {};

struct Widget : CRTP<Widget> {};
```

### type hierarchy 条目中的模板参数

类模板特化产生的 subtype 会被列出，但其条目名称只带有裸模板名（`Derived`），没有用于区分 `Derived<Foo>` 的模板参数。

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

| 能力                     | 状态     | 问题                                                        |
| ------------------------ | -------- | ----------------------------------------------------------- |
| 基本的工作区符号搜索     | 支持     |                                                             |
| 搜索覆盖整个项目         | 支持     |                                                             |
| 重载区分                 | 部分支持 | [clangd#1344](https://github.com/clangd/clangd/issues/1344) |
| 模糊匹配                 | 不支持   | [clangd#914](https://github.com/clangd/clangd/issues/914)   |
| 部分限定名搜索           | 不支持   | [clangd#550](https://github.com/clangd/clangd/issues/550)   |
| 在枚举作用域下查找枚举值 | 不支持   | [clangd#931](https://github.com/clangd/clangd/issues/931)   |
| 底层声明优先于类型别名   | 不支持   | [clangd#2253](https://github.com/clangd/clangd/issues/2253) |
| 按修饰（链接器）名称搜索 | 不支持   |                                                             |

### 基本的工作区符号搜索

不区分大小写的子串匹配

查询会匹配名称包含它的任何符号（忽略大小写）：函数、类型、枚举值和宏都参与；没有任何匹配的查询返回空列表而不是错误。

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

来自非查询文件的命中

查询会返回来自甚至未在编辑器中打开的项目文件的符号：这里的 `other.h` 保持关闭，因此它的命中由后台索引提供。

`main.cpp`:

```cpp
// query: helper_elsewhere

int local_anchor = 0;
```

`other.h`:

```cpp
void helper_elsewhere() {}
```

### 重载区分

结果中显示参数类型

查询重载名称会找到每个重载，但每个条目只带有裸名称——除非打开两个位置，否则无法区分两个 `process` 结果。

```cpp
// query: process

void process(int value) {}

void process(bool flag, int level) {}
```

### 模糊匹配

面向 camelCase 和 snake_case 的词边界感知评分

目前匹配是不区分大小写的子串测试：`LinLis` 找不到 `LinkedList`，`pcfg` 也找不到 `parse_config`。词边界首字母应该匹配并为每种符号类型（包括宏）评分。

```cpp
// query: LinLis
// query: pcfg

struct LinkedList {};

void parse_config();
```

### 部分限定名搜索

符号目前只按裸名称匹配：即使 `deep::net::Socket` 存在，`net::Socket` 也找不到任何结果，其他带限定符前缀的形式也一样。

```cpp
// query: net::Socket

namespace deep {
namespace net {

struct Socket {};

}  // namespace net
}  // namespace deep
```

### 在枚举作用域下查找枚举值

`Color::Red` 应该能找到枚举值——无论 scoped 还是 unscoped 枚举——但带限定符的查询匹配不到任何结果；只有裸 `Red` 可以。

```cpp
// query: Color::Red

enum Color { Red, Green };
```

### 底层声明优先于类型别名

当 `ConnectionImpl` 及其别名 `Connection` 同时匹配查询时，
底层声明应排在前面。当前结果没有排序。

```cpp
// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
```

### 按修饰（链接器）名称搜索

粘贴如 `_Z7processi` 这样的链接器符号，应解析到它修饰的函数——在追踪链接错误和堆栈时很有用。

```cpp
// query: _Z7processi

void process(int value);
```

<!-- END GENERATED ITEMS -->

## Module 导航

<!-- BEGIN GENERATED ITEMS: module_navigation -->

| 能力                                      | 状态     | 问题                                                        |
| ----------------------------------------- | -------- | ----------------------------------------------------------- |
| `import module_name` 导航到模块接口单元   | 支持     | [clangd#2310](https://github.com/clangd/clangd/issues/2310) |
| `import :partition` 导航到 partition 单元 | 支持     |                                                             |
| 在同一模块的接口单元与实现单元之间导航    | 部分支持 |                                                             |
| 点分隔模块名                              | 部分支持 |                                                             |

### `import module_name` 导航到模块接口单元

在 `import` 声明中的名称上执行 go-to-definition 会打开导出它的模块接口单元；对导入符号的使用会到达该单元中的定义。

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

### `import :partition` 导航到 partition 单元

在 partition 导入中冒号后的 partition 名称上执行 go-to-definition 会打开声明它的 partition 单元。

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

### 在同一模块的接口单元与实现单元之间导航

在实现单元（`module m;`）中的模块名称上执行 go-to-definition 会跳到声明该模块的接口单元；反向方向（从接口名称到实现）未提供。

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

### 点分隔模块名

导航每个段

在点分隔模块名的首个段上执行 go-to-definition 会到达该模块的接口单元；点后的段目前还不能自行解析。

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

## 文档高亮

高亮当前文件中光标所在符号的所有引用（`textDocument/documentHighlight`）。

<!-- BEGIN GENERATED ITEMS: document_highlight -->

| 能力                               | 状态   | 问题                                                        |
| ---------------------------------- | ------ | ----------------------------------------------------------- |
| 高亮当前文件中光标下符号的每个引用 | 不支持 |                                                             |
| 符号高亮的读/写分类                | 不支持 |                                                             |
| 控制流 token 高亮                  | 不支持 | [clangd#1921](https://github.com/clangd/clangd/issues/1921) |

### 高亮当前文件中光标下符号的每个引用

将光标放在 `total` 上应点亮它的声明和文件中的每个使用；该请求尚未实现。

```cpp
int total = 0;

void accumulate(int amount) {
    total = total + amount;
}
```

### 符号高亮的读/写分类

每个高亮应携带其访问类型，使编辑器能将写操作与读操作区分着色。

```cpp
void tally() {
    int count = 0;      // write
    int next = count;   // read
    count = next;       // write
}
```

### 控制流 token 高亮

高亮 `break` 或 `continue` 也应点亮它所属的循环或 `switch`——以及 `return` / `throw` 标记的函数退出点。

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

从 `widget.cpp` 一条命令应跳到 `widget.h` 并返回——clangd 客户端依赖的 `textDocument/switchSourceHeader` 请求尚未实现。

```cpp
// widget.h
class Widget {
    void draw();
};

// widget.cpp — #include "widget.h"
void Widget::draw() {}
```

<!-- END GENERATED ITEMS -->
