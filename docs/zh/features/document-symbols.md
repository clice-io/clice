# 文档符号

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

通过 `textDocument/documentSymbol` 提供文件大纲和面包屑导航：嵌套符号树包含范围、选择范围和 `detail` 字段，该字段用于区分重载并显示所声明的类型。

## 符号层级

<!-- BEGIN GENERATED ITEMS: symbol_hierarchy -->

| 能力                   | 状态   | 问题                                                      |
| ---------------------- | ------ | --------------------------------------------------------- |
| 嵌套符号树             | 支持   |                                                           |
| 符号范围和选择范围     | 支持   |                                                           |
| 访问说明符分组         | 不支持 | [clangd#499](https://github.com/clangd/clangd/issues/499) |
| 匿名作用域和内联作用域 | 支持   |                                                           |
| UTF-16 位置编码        | 支持   |                                                           |

### 嵌套符号树

符号按书写时所在的作用域嵌套；类外定义出现在其词法位置，并使用限定名称

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

### 符号范围和选择范围

范围涵盖整个声明；选择范围涵盖完整的书写名称，包括 `~Widget`、`operator==` 和 `operator bool` 这样的多 Token 名称

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

### 访问说明符分组

将 `public:` / `private:` / `protected:` 作为面包屑导航的分组节点

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

### 匿名作用域和内联作用域

匿名命名空间、未命名结构体和联合体将其成员归入占位名称下；内联命名空间的成员仍位于内联命名空间节点下

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

### UTF-16 位置编码

非 ASCII 文本之后的列按 UTF-16 代码单元计数

```cpp
// π ≈ 3.14159, 中文注释
constexpr double 半径 = 2.0;
constexpr double π值 = 3.14159; double area();
```

<!-- END GENERATED ITEMS -->

## 符号种类

<!-- BEGIN GENERATED ITEMS: symbol_kinds -->

| 能力                | 状态     | 问题                                                              |
| ------------------- | -------- | ----------------------------------------------------------------- |
| 核心符号种类        | 支持     |                                                                   |
| 模板声明            | 支持     |                                                                   |
| 模板特化和推导指引  | 支持     |                                                                   |
| 类型别名            | 支持     |                                                                   |
| 显式实例化指令      | 部分支持 | [llvm#191658](https://github.com/llvm/llvm-project/issues/191658) |
| 宏定义              | 支持     | [clangd#1744](https://github.com/clangd/clangd/issues/1744)       |
| Preamble 区域中的宏 | 部分支持 |                                                                   |

### 核心符号种类

命名空间、类、结构体、联合体、枚举及其成员、函数、变量、字段、结构化绑定和 Lambda 都会出现在大纲中，并映射到相应的 LSP 符号种类

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

### 模板声明

类模板、函数模板和变量模板的 detail 中带有 `template ` 前缀；Concept 和带 `concept auto` 参数的缩写函数模板也会出现

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

### 模板特化和推导指引

类模板和变量模板的显式特化与部分特化会出现在大纲中，其名称包含模板实参；成员嵌套在相应的特化下；推导指引显示推导出的签名

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

### 类型别名

`typedef`、`using` 别名和别名模板会出现在大纲中，其 detail 为 `type alias`

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

### 显式实例化指令

类形式显示为不含子节点的符号；clang 错误地将函数形式和变量形式定位到模板模式处，因此它们不会出现在大纲中

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

### 宏定义

对象式宏和函数式宏定义会出现在大纲中，函数式宏的 detail 为参数列表

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

### Preamble 区域中的宏

前导指令段中的定义会出现在检查路径的大纲中，而服务器的 Preamble 记录尚未显示这些定义

```cpp
#define PREAMBLE_LIMIT 8
#define PREAMBLE_CHECK(cond) (!!(cond))

int after = PREAMBLE_LIMIT;
```

<!-- END GENERATED ITEMS -->

## 符号详细信息

<!-- BEGIN GENERATED ITEMS: symbol_detail -->

| 能力                 | 状态   | 问题                                                                                                                                                                              |
| -------------------- | ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 函数签名             | 支持   | [clangd#520](https://github.com/clangd/clangd/issues/520), [clangd#601](https://github.com/clangd/clangd/issues/601), [clangd#1232](https://github.com/clangd/clangd/issues/1232) |
| 变量和字段类型       | 支持   |                                                                                                                                                                                   |
| 移除默认参数         | 支持   | [clangd#221](https://github.com/clangd/clangd/issues/221)                                                                                                                         |
| 在详细信息中显示基类 | 不支持 |                                                                                                                                                                                   |
| 多行签名范围         | 支持   | [clangd#2221](https://github.com/clangd/clangd/issues/2221)                                                                                                                       |
| 带作用域的类型       | 支持   |                                                                                                                                                                                   |

### 函数签名

`detail` 字段中的参数类型和返回类型用于区分重载；构造函数省略 `void` 返回类型

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

### 变量和字段类型

`detail` 字段中的声明类型；Lambda 渲染为 `(lambda)`

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

### 移除默认参数

签名由函数类型派生，因此默认参数值绝不会出现在大纲中

```cpp
namespace detail {

void open_file(const char* path, int mode = 0644);

struct Server {
    void listen(int port = 8080, int backlog = 128);
};

}  // namespace detail
```

### 在详细信息中显示基类

在派生类声明上显示 `: Shape`

```cpp
struct Shape {};

struct Circle : Shape {
    double radius;
};
```

### 多行签名范围

符号范围从声明开头开始并涵盖完整签名，因此编辑器的粘滞滚动（sticky scroll）能够正确锚定

```cpp
struct Config {};

void process_data(
    const Config& cfg,
    int flags
) {}
```

### 带作用域的类型

显式写出的类作用域在详细信息中恰好出现一次，无论是嵌套类、模板标识、别名还是待决名

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

## 缺失的符号

<!-- BEGIN GENERATED ITEMS: missing_symbols -->

| 能力                    | 状态   | 问题                                                        |
| ----------------------- | ------ | ----------------------------------------------------------- |
| 包含指令                | 不支持 | [clangd#2226](https://github.com/clangd/clangd/issues/2226) |
| 局部符号                | 支持   | [clangd#616](https://github.com/clangd/clangd/issues/616)   |
| 模块声明                | 不支持 |                                                             |
| `#pragma mark` 导航标记 | 不支持 |                                                             |
| 友元函数定义            | 支持   |                                                             |

### 包含指令

大纲中的 `#include` 条目

```cpp
#include "config.h"

int uses_config();
```

### 局部符号

函数体内声明的变量和类型作为子项嵌套在所属函数下

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

### 模块声明

大纲中的 `export module`、`module` 和 `import` 声明

```cpp
export module app.core;

import std;

export int core_entry();
```

### `#pragma mark` 导航标记

以大纲条目形式显示的编辑器分节标记

```cpp
#pragma mark - Lifecycle

void setup();

#pragma mark - Rendering

void draw();
```

### 友元函数定义

在类中以内联方式定义的友元函数显示在该类之下

```cpp
struct Owner {
    friend void inline_friend(Owner& o) {}

    friend bool operator==(const Owner& lhs, const Owner& rhs) {
        return &lhs == &rhs;
    }
};
```

<!-- END GENERATED ITEMS -->

## 符号标签

<!-- BEGIN GENERATED ITEMS: symbol_tags -->

| 能力                 | 状态   | 问题                                                        |
| -------------------- | ------ | ----------------------------------------------------------- |
| 弃用标签             | 不支持 |                                                             |
| 访问权限和存储指示符 | 不支持 | [clangd#2123](https://github.com/clangd/clangd/issues/2123) |

### 弃用标签

用 LSP 的 `deprecated` 符号标签标记 `[[deprecated]]` 符号

```cpp
[[deprecated("use open_v2")]] void open_v1();

void open_v2();
```

### 访问权限和存储指示符

大纲条目上的 public / private / protected、static、virtual 和 abstract 标记

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

## 位置准确性

<!-- BEGIN GENERATED ITEMS: location_correctness -->

| 能力               | 状态 | 问题                                                        |
| ------------------ | ---- | ----------------------------------------------------------- |
| 宏展开产生的符号   | 支持 | [clangd#475](https://github.com/clangd/clangd/issues/475)   |
| 宏实参中写出的名称 | 支持 | [clangd#1941](https://github.com/clangd/clangd/issues/1941) |

### 宏展开产生的符号

宏调用生成的符号定位于调用处，而不是宏定义处

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

### 宏实参中写出的名称

选择范围指向宏实参中写出的名称；宏体中写出的名称则回退到调用处

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
