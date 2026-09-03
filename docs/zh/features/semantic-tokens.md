# 语义 Token

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 使用自有的 Token 类型体系对文档中的每个 Token 进行分类；这套体系比标准 LSP Token 类型更丰富，并在 clice 的所有响应中保持一致。偏好标准 LSP 类型的客户端可以通过配置进行映射。

## 词法 Token

从 Token 流本身派生的类型，不依赖 AST。

<!-- BEGIN GENERATED ITEMS: lexical_tokens -->

| 能力                 | 状态   | 问题                                                        |
| -------------------- | ------ | ----------------------------------------------------------- |
| 注释                 | 支持   |                                                             |
| 字面量               | 支持   |                                                             |
| 关键字               | 支持   |                                                             |
| 预处理指令           | 支持   |                                                             |
| 非活跃区域           | 支持   |                                                             |
| 头文件名             | 支持   |                                                             |
| 文件顶部的非活跃区域 | 支持   |                                                             |
| 字面量前缀和后缀     | 不支持 |                                                             |
| 转义序列             | 不支持 |                                                             |
| 声明符与运算符的区分 | 不支持 | [clangd#1421](https://github.com/clangd/clangd/issues/1421) |
| 基本类型 Token       | 支持   |                                                             |
| 括号 Token 类型      | 不支持 |                                                             |

### 注释

行注释、块注释和文档注释，包括多行块注释

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

### 字面量

数字、字符和字符串，包括原始字符串

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

### 关键字

包括运算符的替代拼写以及具有上下文含义的 `final` / `override`

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

### 预处理指令

`#if` 链保留指令类型；未启用的分支保留词法类型；pragma 参数保持普通类型

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

### 非活跃区域

未选中分支中的 Token 保留其词法类型，并带有 `inactive` 修饰符；未分类的 Token 则作为普通的 `identifier` 载体，因此即使单独一行只有 `}` 也会变暗

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

### 头文件名

由引号或尖括号括起的 `#include` 文件名，包括拆分形式 `# include`

```cpp
#include "inc/angled.h"
#include <angled.h>
# include "inc/angled.h"

int after_includes = 0;
```

### 文件顶部的非活跃区域

前导指令中未选中的分支也会以相同方式变暗

```cpp
#define KEEP 1
#if 0
#define DEAD 2
#endif

int after = KEEP;
```

### 字面量前缀和后缀

将编码前缀、类型后缀、数位分隔符和 UDL 后缀分别作为独立 Token

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

### 转义序列

在字符串和字符字面量内单独高亮

```cpp
const char* escaped = "hello\nworld";
char hex_escape = '\x41';
```

### 声明符与运算符的区分

区分 `*`、`&`、`&&` 用作声明符和用作算术/逻辑运算符的情况

```cpp
int value = 1;
int* pointer = &value;
int& reference = value;
int product = value * value;
int masked = value & 1;
```

### 基本类型 Token

为内置类型使用独立的 Token 类型，而非普通的 `keyword`

```cpp
int number = 0;
float ratio = 0.5f;
void act();
unsigned long long wide_number = 0;
__int128 extended_int = 0;
_Float16 extended_float = 0;
```

### 括号 Token 类型

将相互匹配的 `()`、`[]`、`{}`、`<>` 分别归为不同类型

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

## 声明与引用

根据名称所定义或引用的声明进行分类。

<!-- BEGIN GENERATED ITEMS: declarations_references -->

| 能力                              | 状态     | 问题                                                                                                                 |
| --------------------------------- | -------- | -------------------------------------------------------------------------------------------------------------------- |
| 命名空间                          | 支持     |                                                                                                                      |
| 类型                              | 支持     |                                                                                                                      |
| 函数与方法                        | 支持     |                                                                                                                      |
| 变量                              | 支持     |                                                                                                                      |
| 模板                              | 支持     |                                                                                                                      |
| 概念                              | 支持     |                                                                                                                      |
| 标签                              | 支持     |                                                                                                                      |
| 结构化绑定（structured bindings） | 支持     |                                                                                                                      |
| 成员初始化列表                    | 支持     | [clangd#122](https://github.com/clangd/clangd/issues/122)                                                            |
| using 声明                        | 支持     | [clangd#2619](https://github.com/clangd/clangd/issues/2619)                                                          |
| Lambda 初始化捕获                 | 支持     | [clangd#868](https://github.com/clangd/clangd/issues/868)                                                            |
| `sizeof...`                       | 支持     | [clangd#213](https://github.com/clangd/clangd/issues/213)                                                            |
| `using enum`                      | 支持     | [clangd#1283](https://github.com/clangd/clangd/issues/1283)                                                          |
| 推导指引                          | 支持     |                                                                                                                      |
| 显式实例化                        | 支持     | [clangd#316](https://github.com/clangd/clangd/issues/316)                                                            |
| 依赖名称                          | 部分支持 | [clangd#154](https://github.com/clangd/clangd/issues/154), [clangd#297](https://github.com/clangd/clangd/issues/297) |
| 变量模板                          | 支持     |                                                                                                                      |
| 类外成员定义                      | 支持     |                                                                                                                      |
| 别名模板                          | 支持     |                                                                                                                      |
| 模板模板参数                      | 支持     |                                                                                                                      |
| Lambda 捕获                       | 支持     |                                                                                                                      |
| 范围 for 循环                     | 支持     |                                                                                                                      |
| 枚举底层类型                      | 支持     |                                                                                                                      |
| 友元声明                          | 支持     |                                                                                                                      |
| 依赖 using 声明                   | 部分支持 |                                                                                                                      |
| 函数显式实例化指令                | 部分支持 | [llvm#191658](https://github.com/llvm/llvm-project/issues/191658)                                                    |
| 变量的显式实例化指令              | 部分支持 | [llvm#191658](https://github.com/llvm/llvm-project/issues/191658)                                                    |
| 显式实例化的成员函数体            | 支持     |                                                                                                                      |

### 命名空间

定义、引用、嵌套命名空间和命名空间别名

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

### 类型

类、结构体、联合体、枚举和类型别名，包括定义处和引用处

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

### 函数与方法

声明、定义和调用点

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

### 变量

全局变量、局部变量、参数、字段和枚举成员

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

### 模板

类型与非类型模板参数，模板名称带有 `templated` 修饰符

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

### 概念

定义及作为模板约束的使用

```cpp
template <typename T>
concept Small = sizeof(T) <= 4;

template <Small T>
void use_small(T value);

template <typename T>
    requires Small<T>
void require_small(T value);
```

### 标签

`goto` 目标和标签定义

```cpp
void retry(bool again) {
    goto done;
done:
    if (again) {
        goto done;
    }
}
```

### 结构化绑定（structured bindings）

定义处和使用处的绑定名称

起始 `[` 刻意不带 Token；仅高亮绑定名称本身。

```cpp
struct Pair {
    int first, second;
};

void unpack() {
    auto [a, b] = Pair{1, 2};
    int sum = a + b;
}
```

### 成员初始化列表

将被初始化的字段按字段高亮

```cpp
struct Widget {
    int width;
    int height;

    Widget(int w, int h) : width(w), height(h) {}
};
```

### using 声明

引入的名称保留其目标的类别

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

### Lambda 初始化捕获

捕获的名称高亮为变量

```cpp
int compute();

auto fn = [val = compute()] {
    return val;
};
```

### `sizeof...`

参数包保留其类型参数 Token

```cpp
template <typename... Ts>
constexpr auto count = sizeof...(Ts);
```

### `using enum`

在 using 位置高亮枚举名

```cpp
enum class Color { Red };

void paint() {
    using enum Color;
    auto c = Red;
}
```

### 推导指引

高亮推导指引名称及其所指引的模板

```cpp
template <typename T>
struct Vec {
    template <typename It>
    Vec(It first, It last);
};

template <typename It>
Vec(It, It) -> Vec<int>;
```

### 显式实例化

在 extern 声明和定义中，均高亮实例化的模板名称及显式写出的模板实参

```cpp
struct Widget {};

template <typename T>
struct Holder {
    T value;
};

extern template struct Holder<Widget>;

template struct Holder<Widget>;
```

### 依赖名称

若主模板已知，则通过主模板解析

已知模板（`Box<T>`）的依赖成员会解析到主模板的声明，并保留其类别。裸模板参数的成员没有候选声明，目前不会获得 Token；此类名称的启发式着色仍待解决。

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

### 变量模板

声明、定义、偏特化和全特化

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

### 类外成员定义

限定名保持方法的类别和修饰符

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

### 别名模板

别名名称带有类型类别和 `templated` 修饰符

```cpp
template <typename T>
using Ptr = T*;

template <typename T>
struct Box {};

template <typename T>
using BoxPtr = Box<T>*;

Ptr<int> pointer = nullptr;
```

### 模板模板参数

作为类型声明和使用

```cpp
template <typename T>
struct Holder {};

template <template <typename> class Container, typename T>
struct Adaptor {
    Container<T> value;
};

Adaptor<Holder, int> adaptor;
```

### Lambda 捕获

按拷贝和按引用捕获引用被捕获的变量；`this` 仍为关键字

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

### 范围 for 循环

定义处和使用处的循环变量

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

### 枚举底层类型

枚举基类型的引用保留其类型类别

```cpp
using Byte = unsigned char;

enum class Flags : Byte { A, B };

Flags flags = Flags::A;
```

### 友元声明

被声明为友元的名称解析到其目标；内联友元构成定义

```cpp
struct Widget;
void ping();

struct Host {
    friend struct Widget;
    friend void ping();
    friend void inline_friend() {}
};
```

### 依赖 using 声明

模板体中的 `using T::name`

引入的名称及其使用当前不会产生 Token；预留的 dependent-name 修饰符尚未输出。

```cpp
template <typename T>
struct Derived : T {
    using T::value;

    int use() {
        return value;
    }
};
```

### 函数显式实例化指令

Clang 不会为该指令构建节点，因此其中的每个标识符都不会着色：名称、模板实参和参数类型

```cpp
struct Widget {};

template <typename T>
void convert(T value) {}

extern template void convert<Widget>(Widget);

template void convert<Widget>(Widget);
```

### 变量的显式实例化指令

Clang 不会为该指令构建节点，因此其中的每个标识符都不会着色：名称、模板实参，甚至声明符的类型

```cpp
struct Widget {};

template <typename T>
T zero = T();

extern template Widget zero<Widget>;

template Widget zero<Widget>;
```

### 显式实例化的成员函数体

依赖名按其实际解析结果着色：类别一致时保留所有实例化共有的修饰符，类别不一致时标为冲突

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

## 模块

<!-- BEGIN GENERATED ITEMS: modules -->

| 能力                              | 状态 | 问题 |
| --------------------------------- | ---- | ---- |
| 模块声明                          | 支持 |      |
| 模块分区                          | 支持 |      |
| 用作标识符的 `module` 和 `import` | 支持 |      |

### 模块声明

上下文关键字 `module`、以点分隔的模块名和私有片段

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

### 模块分区

模块声明中的分区名称

```cpp
export module demo.core:part;

export int partition_value = 1;
```

### 用作标识符的 `module` 和 `import`

上下文关键字在模块声明之外保持其语义类别

```cpp
void f() {
    struct module {};
    module m;
    int import = 1;
    int module = 2;
}
```

<!-- END GENERATED ITEMS -->

## Token 修饰符

<!-- BEGIN GENERATED ITEMS: token_modifiers -->

| 能力           | 状态   | 问题                                                        |
| -------------- | ------ | ----------------------------------------------------------- |
| 声明与定义     | 支持   |                                                             |
| 静态           | 支持   |                                                             |
| 只读           | 支持   |                                                             |
| 虚与抽象       | 支持   |                                                             |
| 已弃用         | 支持   |                                                             |
| 默认库         | 支持   |                                                             |
| 作用域修饰符   | 不支持 | [clangd#352](https://github.com/clangd/clangd/issues/352)   |
| 可变引用与指针 | 不支持 | [clangd#839](https://github.com/clangd/clangd/issues/839)   |
| 推导           | 不支持 |                                                             |
| 用户定义运算符 | 不支持 | [clangd#1521](https://github.com/clangd/clangd/issues/1521) |

### 声明与定义

该修饰符用于区分两者

```cpp
int measure(int value);

int measure(int value) {
    return value;
}

struct Sensor;

struct Sensor {};
```

### 静态

类级成员和静态局部变量

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

### 只读

const 和 constexpr 值、const 方法及枚举成员

只读目前基于值判定：指向 const 的指针也算作
只读，即使指针本身可以改变。

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

### 虚与抽象

虚方法、纯虚方法和抽象类

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

### 已弃用

`[[deprecated]]` 声明及其使用

```cpp
[[deprecated("use next_api")]] void old_api();
void next_api();

void migrate() {
    old_api();
}
```

### 默认库

系统头文件中声明的符号

```cpp
int before_includes = 0;

#include <syslib.h>

int used = system_helper();
```

### 作用域修饰符

函数、类、文件和全局作用域

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

### 可变引用与指针

通过非 const 引用或指针传递的参数

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

### 推导

标记 `auto` 和 `decltype` 等推导类型

```cpp
auto deduced_int = 1;
decltype(deduced_int) same_type = 2;
```

### 用户定义运算符

区分重载运算符与内置运算符

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

## 冲突与歧义

C++ 允许结构不同的实体共享同一个名称。当源码中的一个名称同时指代不同类别的实体时，任何单一的 Token 类型都无法准确表示；这类名称会获得专用的 **conflict** Token 类型，客户端通常以中性颜色显示。

<!-- BEGIN GENERATED ITEMS: conflict_ambiguity -->

| 能力           | 状态 | 问题 |
| -------------- | ---- | ---- |
| 类型与函数     | 支持 |      |
| 类型与变量     | 支持 |      |
| 同类别重载集合 | 支持 |      |
| 注入类名       | 支持 |      |

### 类型与函数

同时指代二者的名称显示为 `conflict`

```cpp
namespace shop {
struct Widget {};
void Widget();
}

using shop::Widget;
```

### 类型与变量

同时指代二者的名称显示为 `conflict`

```cpp
namespace mixed {
struct Thing {};
int Thing;
}

using mixed::Thing;
```

### 同类别重载集合

仅指代函数的名称不构成冲突

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

### 注入类名

类内部用作构造函数调用的类名

书写的名称显示为类；其隐含的构造函数引用不会额外着色——`(` 不会被标记为 Token。

```cpp
struct Widget {
    Widget(int size);

    Widget create() {
        return Widget(42);
    }
};
```

<!-- END GENERATED ITEMS -->

## Token 正确性

clice 有意固定这些表现形式，其中也包括 clangd 曾处理错误的情况。

<!-- BEGIN GENERATED ITEMS: token_correctness -->

| 能力                     | 状态 | 问题                                                                                                                                                                                |
| ------------------------ | ---- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 构造函数与析构函数       | 支持 | [clangd#1509](https://github.com/clangd/clangd/issues/1509), [clangd#2078](https://github.com/clangd/clangd/issues/2078), [clangd#872](https://github.com/clangd/clangd/issues/872) |
| 匿名参数                 | 支持 |                                                                                                                                                                                     |
| 运算符名称               | 支持 |                                                                                                                                                                                     |
| 类模板的析构函数         | 支持 |                                                                                                                                                                                     |
| 转换运算符               | 支持 |                                                                                                                                                                                     |
| 模板参数上的伪析构函数   | 支持 |                                                                                                                                                                                     |
| 显式预置和弃置的成员函数 | 支持 |                                                                                                                                                                                     |

### 构造函数与析构函数

带有 constructor/destructor 修饰符的 method Token

析构函数名称呈现为两个 Token：`~` 带有 method 类别及 declaration/definition 修饰符，其后的类名仍是对该类的引用。

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

### 匿名参数

未命名参数不产生 Token

未命名参数类型后的标点不产生 Token。

```cpp
void take_one(int) {}
void take_two(int, char* c) {}
```

### 运算符名称

`operator` 关键字和调用处的标点不作额外着色

运算符的书面名称由关键字和标点组成，因此不会生成名称 Token：`operator` 仍归类为关键字，而调用处不会在运算符符号上生成任何 Token。

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

### 类模板的析构函数

模板中的 `~` 形式保持不变

```cpp
template <typename T>
struct Holder {
    ~Holder();
};

template <typename T>
Holder<T>::~Holder() {}
```

### 转换运算符

转换运算符以关键字书写，发生转换的使用处不额外着色

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

### 模板参数上的伪析构函数

`~` 不着色；类型名保持原有类别

```cpp
template <typename T>
void reset(T* value) {
    value->~T();
}
```

### 显式预置和弃置的成员函数

特殊成员函数名称仍被标为定义 Token

```cpp
struct Session {
    Session() = default;
    Session(const Session&) = delete;
    ~Session() = default;
};
```

<!-- END GENERATED ITEMS -->

## 属性

<!-- BEGIN GENERATED ITEMS: attributes -->

| 能力     | 状态   | 问题                                                        |
| -------- | ------ | ----------------------------------------------------------- |
| 属性名称 | 不支持 | [clangd#2209](https://github.com/clangd/clangd/issues/2209) |

### 属性名称

标准属性和厂商属性，以及它们内部的表达式

```cpp
[[nodiscard]] int compute();
[[deprecated("use v2")]] void old_func();
[[maybe_unused]] int counter = 0;

struct [[gnu::packed]] Packed {};
```

<!-- END GENERATED ITEMS -->

## 宏

宏定义体内的 Token 保持其词法类别；根据宏展开结果对这些 Token 进行着色，则属于未来的展开预览功能。

<!-- BEGIN GENERATED ITEMS: macros -->

| 能力               | 状态   | 问题                                                        |
| ------------------ | ------ | ----------------------------------------------------------- |
| 宏定义与展开       | 支持   |                                                             |
| 展开位置与实参     | 支持   |                                                             |
| 类对象宏与类函数宏 | 不支持 | [clangd#2649](https://github.com/clangd/clangd/issues/2649) |

### 宏定义与展开

```cpp
#define SQUARE(x) ((x) * (x))

[[maybe_unused]] static int squared = SQUARE(4);
```

### 展开位置与实参

展开位置的名称按宏高亮，写出的实参保留其语义高亮，定义体保持词法高亮

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

### 类对象宏与类函数宏

对两种形式使用不同的高亮

```cpp
#define MAX_SIZE 1024
#define CHECK(x) ((x) ? 1 : 0)

int checked = CHECK(MAX_SIZE);
```

<!-- END GENERATED ITEMS -->

## 其他已知缺口

以下是尚无测试用例的精选问题：

- `auto` 参数不应被高亮为模板类型参数
  ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- 成员指针中的嵌套名说明符应有对应的 Token
  ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- `::new` 应保持 `new` 关键字高亮
  ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- 当协程返回类型为模板时，`co_yield` / `co_await` 会丢失高亮
  ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- Token 修饰符应应用于重载运算符的操作数
  ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- 依赖模板名（`obj.template get<int>()`）、通过 `using` 从依赖基类导入的成员，以及重载集中包含不同类别实体的依赖名（[clangd#484](https://github.com/clangd/clangd/issues/484)、
  [clangd#686](https://github.com/clangd/clangd/issues/686)、
  [clangd#1057](https://github.com/clangd/clangd/issues/1057)）

## 非活跃代码区域

未选中的预处理分支内，每个 Token 都带有 `inactive` 修饰符，同时保留其词法类别，因此编辑器可以通过设置该修饰符的样式使区域变暗，而不会丢失原有的语法颜色。死代码中未分类的 Token——裸标识符和普通标点——会以不带样式的 `identifier` 类型发出，从而使整个区域都有 Token 覆盖。clice 的 VS Code 扩展默认会以变暗样式渲染这些区域；其他编辑器则直接为该修饰符设置样式（例如 Neovim 中的 `@lsp.mod.inactive`）。

- [x] 使非活跃预处理分支变暗（[clangd#132](https://github.com/clangd/clangd/issues/132)）
- [x] 正确处理 `#elif` 链中的非活跃区域边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）
- [x] 在非活跃区域内保留语法高亮（[clangd#1664](https://github.com/clangd/clangd/issues/1664)）
- [x] 将非活跃区域与注释明确区分（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）
- [ ] 使不可达代码变暗（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）

## 格式字符串高亮

- [ ] `std::format` / `std::print` 占位符高亮（[clangd#1709](https://github.com/clangd/clangd/issues/1709)）
- [ ] 将无效格式说明符高亮为错误

## 协议支持

- [x] 整篇文档的语义 Token（`textDocument/semanticTokens/full`）
- [x] 采用 UTF-16 增量编码的 Token 位置
- [ ] 基于范围的语义 Token（`textDocument/semanticTokens/range`）——仅计算可见视口内的 Token，这对大文件至关重要
- [ ] 增量更新（`textDocument/semanticTokens/full/delta`）——仅发送自上次响应以来的更改
