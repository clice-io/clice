# 悬停

为光标所在的符号显示内容丰富的信息卡片。

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## 符号信息

<!-- BEGIN GENERATED ITEMS: symbol_information -->

| 能力               | 状态     | 问题                                                        |
| ------------------ | -------- | ----------------------------------------------------------- |
| 限定名             | 支持     |                                                             |
| 符号种类           | 支持     |                                                             |
| 访问说明符         | 支持     |                                                             |
| 定义渲染           | 支持     |                                                             |
| 初始化器截断       | 部分支持 | [clangd#710](https://github.com/clangd/clangd/issues/710)   |
| 虚函数修饰符       | 部分支持 | [clangd#2474](https://github.com/clangd/clangd/issues/2474) |
| 匿名命名空间作用域 | 部分支持 | [clangd#436](https://github.com/clangd/clangd/issues/436)   |

### 限定名

悬停卡片显示所属命名空间和类作用域。

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

### 符号种类

卡片说明符号的种类：struct、enum、function、field 等。

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

### 访问说明符

成员会显示其 public / protected / private 访问级别。

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

### 定义渲染

卡片包含符号在源代码中的定义。

```cpp
namespace retry {

constexpr int max_retries = 3;

int backoff(int attempt = 1) {
    return attempt * max_retries;
}

}
```

### 初始化器截断

过大的初始化器会截断显示，而不会完整显示。

渲染出的定义省略了初始化器，但求值后的 `Value` 字段仍然列出全部 256 个元素。

```cpp
#define A(x) x, x, x, x
#define B(x) A(A(A(A(x))))
int arr[] = {B(0)};
```

### 虚函数修饰符

方法的悬停信息中会显示 `virtual` / `override` / `final`。

源代码中写出的修饰符会渲染出来（`virtual … = 0`、`override`、`final`），但重写方法如果省略了冗余的 `virtual` 关键字，就不会显示任何表明其为虚函数的信息——卡片缺少该 issue 所要求的 `virtual void draw() override` 形式。

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

### 匿名命名空间作用域

作用域信息中会显示 `(anonymous namespace)`。

卡片会正常渲染，但作用域信息会省略匿名部分：顶层匿名命名空间中的成员完全不显示作用域行，`outer::(anonymous)` 则只显示 `outer`。

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

## 类型信息

<!-- BEGIN GENERATED ITEMS: type_information -->

| 能力               | 状态     | 问题                                                        |
| ------------------ | -------- | ----------------------------------------------------------- |
| 变量类型           | 支持     |                                                             |
| 类型别名           | 支持     |                                                             |
| 函数签名           | 支持     |                                                             |
| 模板形参           | 支持     |                                                             |
| `auto` 推导        | 支持     |                                                             |
| `decltype` 推导    | 支持     |                                                             |
| CTAD               | 部分支持 | [clangd#435](https://github.com/clangd/clangd/issues/435)   |
| 实例化实参         | 部分支持 | [clangd#230](https://github.com/clangd/clangd/issues/230)   |
| Lambda `auto` 形参 | 不支持   | [clangd#493](https://github.com/clangd/clangd/issues/493)   |
| 带语法糖的 `auto`  | 支持     |                                                             |
| 类型格式化         | 不支持   | [clangd#2156](https://github.com/clangd/clangd/issues/2156) |
| 匿名结构体 typedef | 支持     | [clangd#2219](https://github.com/clangd/clangd/issues/2219) |
| 概念约束           | 部分支持 |                                                             |

### 变量类型

指针、引用、数组

变量卡片会以易读形式打印其声明类型，并按源码中的写法呈现指针、引用和数组声明符。

```cpp
namespace variable_type {

int target;

int *ptr = &target;

int &ref = target;

int numbers[4]{};

}
```

### 类型别名

脱糖后的 `aka` 形式

保留语法糖的类型会将其底层类型显示为 `Alias (aka int)`。`show_aka` 选项可以关闭 `aka` 后缀。

```cpp
namespace aka_desugar {

using Handle = int;
using Alias = Handle;

Handle direct = 0;

Alias chained = 0;

}
```

### 函数签名

返回类型、形参名、默认值

函数卡片会列出返回类型、各形参及其名称，以及默认实参。

```cpp
namespace function_signature {

int add(int lhs, int rhs);

void configure(int width, bool visible = true);

}
```

### 模板形参

类型、模板模板、非类型

每种模板形参都会显示其形式：类型形参、模板模板形参，以及带默认值的非类型模板形参。

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

### `auto` 推导

占位符解析成的类型

悬停 `auto` 占位符会显示替换它的类型——内置类型、指针、Lambda、模板实例化，以及未实例化模板中的 `/* not deduced */` 标记。

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

### `decltype` 推导

值、引用和依赖形式

悬停 `decltype` 或 `decltype(auto)` 占位符会显示解析出的类型，包括括号表达式规则添加的引用。

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

从类占位符推导出的模板实参

使用类模板实参推导时，变量卡片会显示推导出的 `Box<int>`，但悬停类名本身时，仍只显示不带实参的主模板。

```cpp
namespace ctad_arguments {

template <typename T> struct Box {
  Box(T);
};

Box picked(42);

}
```

### 实例化实参

在使用处绑定的模板形参

模板使用处会显示替换后的类型（`Wrapper<int>`、`identity<int>`、`int x`），但不会显示将每个形参与其所绑定实参对应起来的显式 `T = int` 映射。

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

### Lambda `auto` 形参

推导出的形参类型

悬停泛型 Lambda 的 `auto` 形参不会显示卡片；推导出的形参类型不会显示。

```cpp
namespace lambda_auto_params {

auto printer = [](auto value) { return value; };

}
```

### 带语法糖的 `auto`

推导过程中保留别名语法糖

clangd 以 clangd#709 跟踪 `auto` 丢失别名语法糖的问题；clice 已经能保留别名拼写并附加其脱糖形式，因此从别名返回类型推导出的 `auto` 会显示为 `Outer // aka: int`。

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

### 类型格式化

对渲染后的类型应用 clang-format

长类型或嵌套类型由编译器的默认类型打印器打印；它们不会再经 clang-format 重新换行或对齐。

```cpp
namespace clang_format_types {

template <typename A, typename B, typename C, typename D>
struct Tuple {};

Tuple<int, long, unsigned, char> wide;

}
```

### 匿名结构体 typedef

经典 C `typedef struct {…} Name`

按 C11 编译时，clangd 会将匿名结构体的别名误导性地渲染为 `struct Point`；clice 使用其 typedef 名称来命名该结构体，因此别名本身及该类型的变量都会显示简洁的 `Point` 卡片。

```cpp
/// A 2-D point.
typedef struct {
  int x, y;
} Point;

Point origin = {.y = 2, .x = 1};
```

### 概念约束

形参或 `auto` 占位符背后的约束

受约束形参和概念引用的卡片会包含相应约束，但悬停受约束的 `Addable auto` 变量的占位符时只会显示推导出的类型——约束会被丢弃。

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

## 布局信息

<!-- BEGIN GENERATED ITEMS: layout_information -->

| 能力         | 状态     | 问题                                                        |
| ------------ | -------- | ----------------------------------------------------------- |
| 字段布局     | 支持     |                                                             |
| 类型级布局   | 部分支持 | [clangd#1763](https://github.com/clangd/clangd/issues/1763) |
| 虚函数表偏移 | 部分支持 | [clangd#1771](https://github.com/clangd/clangd/issues/1771) |

### 字段布局

悬停字段时显示大小、偏移、对齐和填充

语料库固定使用 x86-64 目标，因此以位为单位的数值是稳定的。

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

### 类型级布局

悬停类型本身显示其大小、对齐和填充

目前，类型卡片会显示大小和对齐；总填充尚未显示。

```cpp
namespace layout {

struct Widget {
    int id;
    double value;
};

}
```

### 虚函数表偏移

虚函数显示其虚函数表槽位

目前，方法卡片不会渲染任何虚函数表信息。

```cpp
struct Shape {
    virtual void draw();
    virtual void move();
};
```

<!-- END GENERATED ITEMS -->

## 表达式上下文

<!-- BEGIN GENERATED ITEMS: expression_context -->

| 能力         | 状态     | 问题                                                        |
| ------------ | -------- | ----------------------------------------------------------- |
| 常量求值     | 支持     |                                                             |
| 调用实参     | 支持     |                                                             |
| 传递语义     | 支持     |                                                             |
| 隐式转换     | 支持     |                                                             |
| 字符串字面量 | 部分支持 | [clangd#1016](https://github.com/clangd/clangd/issues/1016) |
| 数值字面量   | 不支持   | [clangd#1669](https://github.com/clangd/clangd/issues/1669) |
| 记录类型变量 | 部分支持 | [clangd#1622](https://github.com/clangd/clangd/issues/1622) |

### 常量求值

constexpr、枚举项、sizeof

当初始化器是常量表达式时，卡片会对其求值并显示结果值。

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

### 调用实参

每个实参绑定到哪个形参

在调用点悬停某个实参时，会显示它传递给哪个形参，并给出所绑定形参的名称。

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

### 传递语义

按值、按引用、按 const 引用

实参卡片会说明值以何种方式传给被调用方：按值复制，或绑定到可变引用或 const 引用形参。

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

### 隐式转换

实参转换为形参类型

当实参通过隐式转换传给形参时，卡片会标注目标类型，无论是内建转换还是用户自定义转换。

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

### 字符串字面量

悬停时报告的长度

字符串字面量卡片报告数组类型及其字节大小（`const char[6]`、`Size: 6 bytes`——长度加上 null 终止符），而不是显式的字符数。

```cpp
namespace string_length {

const char *greeting = "hello";

}
```

### 数值字面量

整数或浮点字面量的类型和值

悬停数值字面量时不会显示卡片；字符字面量和字符串字面量则会显示其类型和值。

```cpp
namespace numeric_literal_type {

auto count = 42;
auto ratio = 3.14;

}
```

### 记录类型变量

外层的常量值泄漏进来

悬停可进行常量求值的调用中的记录类型实参时，当前会在该变量上报告调用的值（`Value = 7`）——该值并不属于记录本身。

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

## 文档

<!-- BEGIN GENERATED ITEMS: documentation -->

| 能力                         | 状态     | 问题                                                        |
| ---------------------------- | -------- | ----------------------------------------------------------- |
| Doxygen `///` 注释           | 支持     |                                                             |
| 合成访问器文档               | 支持     |                                                             |
| `@copydoc` 标签              | 部分支持 | [clangd#1320](https://github.com/clangd/clangd/issues/1320) |
| 继承的重写方法文档           | 部分支持 | [clangd#2504](https://github.com/clangd/clangd/issues/2504) |
| 重载文档共享                 | 部分支持 | [clangd#2506](https://github.com/clangd/clangd/issues/2506) |
| 继承构造函数文档             | 不支持   | [clangd#1936](https://github.com/clangd/clangd/issues/1936) |
| 横幅注释                     | 部分支持 | [clangd#974](https://github.com/clangd/clangd/issues/974)   |
| 声明与定义注释               | 支持     |                                                             |
| 空白和换行                   | 部分支持 | [clangd#2057](https://github.com/clangd/clangd/issues/2057) |
| 注释缩进                     | 部分支持 | [clangd#1040](https://github.com/clangd/clangd/issues/1040) |
| 宏展开产生的 template 关键字 | 部分支持 | [clangd#1226](https://github.com/clangd/clangd/issues/1226) |
| 注释隐藏选项                 | 不支持   | [clangd#2148](https://github.com/clangd/clangd/issues/2148) |

### Doxygen `///` 注释

从声明中提取，并在悬停时渲染

适用于普通函数、主模板及其特化；符号引用会解析为特化程度最高的声明所带的注释。

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

### 合成访问器文档

为简单 getter/setter 生成一行描述

自身没有注释的简单 getter 或 setter，其悬停卡片中会生成一行“Trivial accessor/setter for `field`.”。

```cpp
namespace accessors {
struct Widget {
    int width;
    int getWidth() { return width; }
    void setWidth(int w) { width = w; }
};
}
```

### `@copydoc` 标签

将另一个符号的文档复制到当前符号上

`@copydoc target` 标签应将 `target` 的文档复制到当前符号的悬停卡片中。clice 目前还不解析该标签——卡片显示的是字面文本 `@copydoc base_func()`。

```cpp
namespace copydoc {
/// Detailed documentation.
void base_func();

/// @copydoc base_func()
void wrapper();
}
```

### 继承的重写方法文档

没有注释的重写方法应显示基类方法的文档

悬停在自身没有注释的重写方法上时，应显示其所重写方法的文档。clice 目前还不继承该文档——重写方法的卡片中没有任何描述。

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

### 重载文档共享

后续没有注释的重载应复用第一个重载的文档

连续的重载通常只为第一个编写文档；后续未写文档的重载应复用这份共享描述。clice 目前还不共享该文档——后续重载的卡片中没有任何描述。

```cpp
namespace overloads {
/// Opens a file.
void open(const char* path);
void open(const char* path, int flags);
}
```

### 继承构造函数文档

`using Base::Base;` 应显示基类构造函数的文档

通过 `using Base::Base;` 引入的构造函数在悬停时应带有基类构造函数的文档。但目前没有可供悬停的位置：using 声明中的名字解析到类，而不是继承的构造函数。

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

### 横幅注释

与下一个声明之间有空行的分节横幅不应附着到该声明

`// ==== Section ====` 这样的横幅后跟空行时，不应被误当作其下声明的文档。clice 目前仍会将它附着到声明上——横幅文本会出现在卡片中。

```cpp
namespace banners {
// ==== Section Banner ====

void foo();
}
```

### 声明与定义注释

优先使用声明的文档，而不是定义处的注释

clangd 将其跟踪为 clangd#829；clice 已经优先使用声明处的 `///` 文档，而不是定义处的普通 `//` 注释，并在声明处和定义处都显示它。

```cpp
namespace decldef {
/// Public API documentation.
void process(int x);

// Internal implementation note.
void process(int x) { (void)x; }
}
```

### 空白和换行

注释中的 Markdown 表格保留其换行

用多行 `///` 编写的 Markdown 表格应在保留换行的情况下渲染为表格。clice 目前将这些行压成一行，因此表格无法渲染。

```cpp
namespace tables {
/// | Column A | Column B |
/// |----------|----------|
/// | 1        | 2        |
void table_fn();
}
```

### 注释缩进

注释中的缩进行渲染时不应出现多余缩进

正文中包含缩进块的文档注释应以正确的缩进渲染。clice 目前会去掉前导缩进，导致缩进代码块失去缩进，空行也会塌缩。

```cpp
namespace indented {
/// Summary line.
///
///     step_one();
///     step_two();
void run();
}
```

### 宏展开产生的 template 关键字

文档字符串应在展开后保留

当 `template` 关键字来自宏展开时，声明的文档注释仍应出现在悬停卡片中。clice 目前会丢失该注释——卡片中没有描述。

```cpp
int anchor = 0;

#define TEMPLATE template

/// A documented template function.
TEMPLATE <typename T> void run(T value);
```

### 注释隐藏选项

用于隐藏错误归属文档注释的配置开关

被关联启发式规则误拾取的无关注释——例如与代码之间隔有空行的分节横幅——总会出现在悬停卡片中：clice 没有配置选项，可隐藏那些仅凭推测与代码关联的文档注释。

```cpp
namespace suppression {
// TODO: tidy this file up.

int counter;
}
```

<!-- END GENERATED ITEMS -->

## 宏悬停

<!-- BEGIN GENERATED ITEMS: macro_hover -->

| 能力                    | 状态     | 问题                                                        |
| ----------------------- | -------- | ----------------------------------------------------------- |
| 各处的定义文本          | 支持     |                                                             |
| 完全展开预览            | 支持     |                                                             |
| 命令行宏                | 支持     |                                                             |
| 实参中的嵌套宏          | 部分支持 |                                                             |
| 定义前使用              | 部分支持 | [clangd#2642](https://github.com/clangd/clangd/issues/2642) |
| Preamble 内的 `#define` | 不支持   |                                                             |

### 各处的定义文本

`#define`、使用处、`#ifdef` 和 `#undef` 都显示宏的定义

宏的悬停卡片在其名称出现的任何地方都带有 `#define` 文本：定义本身、使用处、`#ifdef` 守卫和 `#undef`。

```cpp
int anchor = 0;

#define LIMIT 64

int use = LIMIT;

#ifdef LIMIT
int guarded = 1;
#endif

#undef LIMIT
```

### 完全展开预览

函数式宏的使用处显示将实参代入宏体后的结果

悬停函数式宏调用会显示 `#define` 文本，以及代入调用实参后完全展开的结果预览。

```cpp
int x = 1, y = 2;

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int z = MAX(x, y);
```

### 命令行宏

悬停 `-D` 定义时会显示合成的 `#define`

通过命令行定义的宏（`-DFROM_CLI=7`）会在悬停卡片中显示合成的 `#define FROM_CLI 7`，然后显示其展开结果。

```cpp
int cli = FROM_CLI;
```

### 实参中的嵌套宏

在另一宏调用的实参中出现的宏

记录的展开从外层调用开始，因此悬停实参中出现的内层宏时，只会显示其定义，不会显示展开预览。

```cpp
int anchor = 0;

#define ECHO(x) x
#define INNER_VAL 99

int nested = ECHO(INNER_VAL);
```

### 定义前使用

悬停出现在其 `#define` 之前的宏名

在宏自身的 `#define` 之前通过 `#if` 使用该宏名时，悬停仍应显示宏的定义。clice 目前不会在定义前的使用处提供悬停信息；`#define` 之后的使用则正常。

```cpp
int anchor = 0;

#if COUNT > 0
int positive = 1;
#endif

#define COUNT 3

int use = COUNT;
```

### Preamble 内的 `#define`

悬停文件开头的指令

文件的 Preamble 区域（第一个声明之前连续出现的一组指令）中的 `#define` 不属于当前解析的预处理器记录，因此悬停其名称不会显示任何内容。其他所有宏测试样例都特意以声明开头，以便将其中的指令推到 Preamble 边界之后。

```cpp
#define EARLY 1

int use = EARLY;
```

<!-- END GENERATED ITEMS -->

## 特殊悬停目标

<!-- BEGIN GENERATED ITEMS: special_hover_targets -->

| 能力                        | 状态     | 问题                                                        |
| --------------------------- | -------- | ----------------------------------------------------------- |
| 类型悬停时的成员            | 部分支持 | [clangd#959](https://github.com/clangd/clangd/issues/959)   |
| typedef 的底层结构体        | 部分支持 | [clangd#2020](https://github.com/clangd/clangd/issues/2020) |
| 关键字文档                  | 不支持   | [clangd#1862](https://github.com/clangd/clangd/issues/1862) |
| 属性文档                    | 支持     | [clangd#1862](https://github.com/clangd/clangd/issues/1862) |
| include 指令悬停            | 支持     |                                                             |
| `this` 表达式               | 支持     |                                                             |
| 预定义标识符                | 支持     |                                                             |
| 无意义 Token 不显示悬停     | 支持     |                                                             |
| GTK-Doc 和 kernel-doc       | 不支持   | [clangd#2662](https://github.com/clangd/clangd/issues/2662) |
| Doxygen 中的 LaTeX 数学公式 | 不支持   | [clangd#2669](https://github.com/clangd/clangd/issues/2669) |

### 类型悬停时的成员

悬停枚举或结构体类型时列出其成员

卡片会显示类型名称（结构体还会显示其布局），但不会展开成员列表——类型体显示为 `{}`。

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

### typedef 的底层结构体

悬停别名时展开别名所指的定义

卡片会将别名解析为其底层类型名，但不会展开该结构体的定义或成员列表。

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

### 关键字文档

悬停语言关键字时显示其描述

悬停 `const` 或 `virtual` 等关键字时不会显示卡片。

```cpp
namespace keywords {

const int limit = 42;

struct Widget {
    virtual void draw();
};

}
```

### 属性文档

悬停属性时显示其描述

该属性自身的文档会显示在卡片中，GNU `__attribute__` 写法和 C++ `[[...]]` 属性均支持。

```cpp
namespace attr_docs {
void foo(int * __attribute__((nonnull, noescape)) );

[[nodiscard]] int compute();
}
```

### include 指令悬停

悬停 `#include` 时显示解析后的头文件路径

卡片会将引号中的头文件解析为其在磁盘上的文件路径。

```cpp
#include "own_header.h"

int use = own_header_value;
```

### `this` 表达式

悬停 `this` 时显示其指向对象的类类型

适用于普通类和类模板内部。

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

### 预定义标识符

悬停 `__func__` 时显示当前函数名

在具体函数中可解析出其值；在模板内只能确定大致类型。

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

### 无意义 Token 不显示悬停

内置关键字和空代码体均不显示卡片

悬停内建类型关键字或空代码体内部时，不会产生任何信息卡，因此编辑器不会显示任何内容，而非显示干扰信息。
（目前数值和 bool 字面量也没有信息卡，但这是已跟踪的缺失——见数值字面量条目——并非有意保证的行为。）

```cpp
namespace negatives {

int counter = 0;

void noop() {}

}
```

### GTK-Doc 和 kernel-doc

识别 GObject Introspection 注解

GTK-Doc / kernel-doc 注释语法和 GObject Introspection 注解不会作为悬停信息卡的内容进行解析。

```cpp
/**
 * gtk_widget_show:
 * @widget: (transfer none): a #GtkWidget
 *
 * Flags a widget to be displayed.
 */
void gtk_widget_show(GtkWidget *widget);
```

### Doxygen 中的 LaTeX 数学公式

渲染 `@f$ ... @f$` 公式

Doxygen LaTeX 数学公式按原文显示，不渲染为数学公式。

```cpp
/// The area of a circle is @f$ A = \pi r^2 @f$.
double circle_area(double r);
```

<!-- END GENERATED ITEMS -->

## 展示

<!-- BEGIN GENERATED ITEMS: presentation -->

| 能力          | 状态 | 问题 |
| ------------- | ---- | ---- |
| Markdown 渲染 | 支持 |      |

### Markdown 渲染

信息卡以 markdown 渲染，或通过 `parse_comment_as_markdown = false` 以纯文本渲染

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

## 模块相关

<!-- BEGIN GENERATED ITEMS: module_related -->

| 能力            | 状态   | 问题 |
| --------------- | ------ | ---- |
| import 语句悬停 | 不支持 |      |
| 模块名悬停      | 不支持 |      |

### import 语句悬停

悬停 `import` 显示模块信息

悬停 `import` 声明目前还不能描述被导入的模块。

```cpp
export module app;

import utils;
```

### 模块名悬停

悬停模块名列出其所属文件

悬停模块名目前还不能列出声明它的文件或分区。

```cpp
export module math;

export module math:algebra;
```

<!-- END GENERATED ITEMS -->

## 悬停正确性

面对曾导致其他工具出错的输入时仍保持稳健。

<!-- BEGIN GENERATED ITEMS: hover_correctness -->

| 能力                                  | 状态 | 问题 |
| ------------------------------------- | ---- | ---- |
| MSVC 继承模型                         | 支持 |      |
| 最令人烦恼的解析（most vexing parse） | 支持 |      |
| 大无符号枚举常量                      | 支持 |      |
| 带默认参数的调用                      | 支持 |      |
| 宏遮蔽的符号                          | 支持 |      |

### MSVC 继承模型

`MSInheritanceAttr` 不会导致类或结构体的悬停信息异常

clangd 将此项跟踪为 clangd#1643 和 clangd#2212；使用 MSVC 目标时，隐式继承属性不会混入类、结构体或方法的信息卡。

```cpp
namespace ms {

struct Widget {
    int value;
    void update();
};

int Widget::* member = &Widget::value;

}
```

### 最令人烦恼的解析（most vexing parse）

悬停信息能区分对象初始化和函数声明

clangd 将此项跟踪为 clangd#2225；clice 将直接初始化识别为变量，将引起歧义的形式识别为函数声明。

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

### 大无符号枚举常量

悬停值为 `0xFFFF...ULL` 的枚举项时不会崩溃

clangd 在此情景下崩溃（clangd#2381）；clice 能够完整呈现无符号值，且不会溢出。

```cpp
namespace big_enum {

enum class Flags : unsigned long long {
    Max = 0xFFFFFFFFFFFFFFFFULL,
};

}
```

### 带默认参数的调用

悬停未显式传入默认实参的调用时不会崩溃

clangd 在此情景下崩溃（clangd#551）；clice 呈现包含默认参数的被调用方签名。

```cpp
namespace defaults {

int compute(int a, int b = 10, int c = 20);

int result = compute(1);

}
```

### 宏遮蔽的符号

遮蔽同名函数的函数式宏

clangd 将此项跟踪为 clangd#2490；在调用点，函数式宏处于活动状态，clice 的信息卡显示该宏及其展开。

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
