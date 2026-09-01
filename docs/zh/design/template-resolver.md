# 模板解析

## 背景

C++ 模板的一个核心设计原则是**延迟实例化**——在模板代码用具体类型实参实例化之前，编译器不会（也无法）解析依赖模板参数的名称。这些名称称为**依赖名称**，在模板定义时其类型未知。

对 language server 来说，这意味着模板代码内部是一个“盲区”：

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.  // cursor here -- vec's type is known to be vector<T>, completions can be provided
}
```

简单情况下，使用主模板的定义就足以提供基本的补全。但考虑一个更复杂的场景：

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].  // What is the type of vec2[0]?
}
```

`vec2[0]` 的类型是 `std::vector<std::vector<T>>::reference`——一个依赖名称。在标准库实现中，解析它需要追踪几十层嵌套的模板 typedef 链：`reference` -> `allocator_traits<Alloc>::value_type` -> `__alloc_traits<Alloc>::reference` -> ...。每一层都涉及偏特化匹配、默认模板实参、typedef 展开等复杂操作。

在 clangd 社区中，模板代码里的补全和 hover 问题长期存在。用户反复报告在模板函数体内无法获得补全建议——光标位置显示 `<dependent type>`，所有 LSP 功能都不可用。根本原因是 clangd 的解析策略有几个根本性限制：

**不处理偏特化**：clangd 假设成员查找总是使用主模板的定义。但标准库大量使用偏特化（例如 `allocator_traits` 针对不同分配器做了偏特化），主模板甚至可能没有要查找的成员——该成员只存在于某个具体的偏特化中。

**缺少实参映射**：即使名称查找找到了成员的类型，该类型仍然以被查找模板的参数（例如 `allocator_traits<Alloc>::value_type` 中的 `Alloc`）表示，而不是调用方的参数（例如 `T`）。由于 clangd 不执行实例化，它无法建立参数之间的映射。

**忽略默认模板实参**：`std::vector<T>` 实际上是 `std::vector<T, std::allocator<T>>`——第二个实参是默认的。clangd 不展开默认实参，导致依赖默认实参的名称无法解析。

## 设计

### 核心思路

clice 实现了一个 **PseudoInstantiator**——它通过启发式方法解析依赖名称，而不需要具体类型实参。核心洞见是：你不需要知道 `T` 是 `int` 还是 `string`；只需要追踪 `T` 在模板 typedef 链中的传播路径，并用 `T` 表达最终结果。

例如：`std::vector<std::vector<T>>::reference` 经过伪实例化简化为 `std::vector<T>&`——一个足够具体、能提供补全的类型，同时保留了模板参数 `T` 的符号含义。

### 一个重写器，两种策略

解析器是一个手写的类型重写器。它有意不使用 Sema 或 Clang 的 TreeTransform——两者都假设存在带有具体实参的真实实例化上下文，而这正是这里缺少的。一个重写器按以下两种策略之一运行：

**解析策略（启发式解析）**

这是主引擎，负责解析各种依赖名称：

- **DependentNameType**（例如 `typename Container::value_type`）：在模板的成员中查找 `value_type`，匹配偏特化，展开 typedef，并用调用方的参数重新表达结果
- **DependentTemplateSpecializationType**（例如 `Alloc::rebind<U>`）：解析依赖的模板特化，首先尝试标准库模式的快速路径
- **TemplateTypeParmType**（例如 `T`）：通过实例化栈查找参数的绑定值或默认实参
- **DecltypeType**（例如 `decltype(var)`）：将简单的变量引用解析为其声明类型

**替换策略（打破循环）**

当启发式解析展开一个 typedef 时，可能会遇到另一个需要启发式查找的依赖名称，从而形成循环：typedef A 的底层类型引用了依赖名称 B，而查找 B 又发现其类型再次涉及 typedef A。

替换策略打破这种循环——它只执行参数替换和 typedef 展开，不做启发式查找。解析需要展开 typedef 时，会委托给替换策略，确保递归启发式查找不会被触发。

### 实例化栈

模板参数在嵌套模板中可能位于不同的深度层级。实例化栈（InstantiationStack）维护参数映射的栈，每个帧记录一层模板参数绑定。

遇到 `TemplateTypeParmType`（通过深度和索引标识的模板参数）时，解析器从栈顶（最内层）向栈底（最外层）线性搜索，在对应深度匹配参数绑定。找到匹配则将该参数替换为绑定类型；未找到则尝试参数的默认值。当栈为空（独立的依赖类型）时，解析器向上遍历外层模板声明，将其注入的模板参数作为上下文压栈。

这使解析器可以处理多层嵌套模板：

```cpp
template<typename X>
struct Outer {
    template<typename Y>
    struct Inner {
        typename std::pair<X, Y>::first_type member;
        // X is at depth 0, Y is at depth 1
        // Both need to be tracked in the stack for correct resolution
    };
};
```

### 依赖名称解析流程

以 `typename A<T>::type` 为例：

1. **缓存检查**：如果此节点之前已解析过，直接返回缓存结果
2. **循环检测**：如果此节点当前正在解析中，中止以防止无限递归
3. **限定符变换**：变换 `A<T>` 部分——替换参数，匹配偏特化
4. **成员查找**：在变换后的类型中查找 `type`。先尝试每个偏特化及其基类，再尝试主模板及其基类
5. **参数推导**：利用 Clang 的模板参数推导机制建立形式参数到实际参数的映射
6. **替换**：通过 SubstituteOnly 展开找到的成员类型中的 typedef，使用调用方的参数进行替换
7. **递归**：如果结果仍包含依赖名称，则递归解析

整个过程有 16 层的递归深度限制，以防止极端情况下的无限递归。

解析器还维护两层循环检测：一层防止同一 DependentNameType 节点的重入解析，另一层防止同一 ClassTemplateDecl 上的递归查找（处理 CRTP 和其他自引用模式）。

### 偏特化匹配

偏特化匹配是伪实例化器相对 clangd 的一个关键优势。标准库大量使用偏特化，例如：

```cpp
// Primary template -- does not define value_type
template<typename Alloc> struct allocator_traits;

// Partial specialization -- defines value_type
template<typename T> struct allocator_traits<allocator<T>> {
    using value_type = T;
};
```

解析 `allocator_traits<allocator<int>>::value_type` 时，clangd 因为只在主模板中查找 `value_type` 而失败。伪实例化器尝试将实际参数与每个偏特化模式进行匹配，找到正确的偏特化后，在其中查找该成员。

### 标准库特殊处理

标准库的分配器重绑定链是一条特别深的 typedef 链：

```text
vector<T> -> __alloc_traits<allocator<T>> -> allocator_traits<allocator<T>>
  -> allocator_traits<Alloc>::rebind_alloc<U>
```

这条链可能超过深度限制。解析器对 `allocator_traits::rebind_alloc` 模式做了短路处理：检测到该模式后，解析器直接尝试 `Alloc::rebind<T>::other`（标准分配器协议），否则回退为替换第一个模板参数。

### 类型重糖化

Clang 内部将模板参数表示为规范化的 `TemplateTypeParmType` 值（深度 + 索引）。用户应该看到参数名称（例如 `T`），而不是 `parameter 0 of depth 0`。ResugarOnly 变换将规范化的参数类型映射回原始声明，生成用户友好的类型信息。

### 优雅降级

如果解析过程中的任何步骤失败（查找失败、循环检测触发、深度超限），解析器返回原始依赖类型而不报错。这意味着 LSP 功能不会因解析失败而彻底不可用——它们只是降级到“无法解析”状态，这并不比完全不执行伪实例化更差。

## 设计决策与权衡

**为什么选择伪实例化而不是真正的模板实例化？** 真正的实例化需要具体的类型实参（例如 `T = int`），但模板定义时这些信息不可用。而且，语言服务器中的代码经常是不完整的，无法进行完整实例化。伪实例化通过保留符号参数（`T` 本身）来避免依赖具体类型。

**为什么要用两阶段设计？** 单阶段设计中，typedef 展开会触发新的启发式查找，而查找结果又可能需要 typedef 展开——形成循环。两阶段设计通过职责分离打破这个循环：启发式查找只在第一阶段执行，typedef 展开交给第二阶段，第二阶段不执行查找。

**为什么要对标准库模式做特殊处理？** 标准库的 allocator rebinding 是实际代码中最常见的深层 typedef 链。通用递归解析在这里会超过深度限制。虽然不够优雅，但特殊处理覆盖了最高频的用户场景。未来计划用通用机制替换这些特殊处理。

**为什么选择优雅降级而不是报错？** 模板解析本质上是一种启发式方法——不可能覆盖 C++ 模板的所有边界情况。优雅降级确保解析失败的最坏情况不会比“完全不解析”更糟，同时在解析成功的情况下带来明显的体验改进。

## 实例化作为实现

伪实例化从模板的书写侧工作：它仅从模式本身推断一个依赖名称 _可能_ 的含义。翻译单元通常持有互补的事实来源——_实际_ 实例化，记录了每个依赖名称对于每个具体实参列表 _确实_ 的含义。

clice 有意将模板视为鸭子类型接口，将每个实例化视为该接口的实现，类似于虚函数与其 override 的关系。这是一个长期方向，并且它已经影响了当前的行为：

- 语义分类不会跳过实例化代码。一个实例化会复用模式中的源码位置，因此实例化体内的依赖名称解析会落到用户写在模板中的那个 token 上。只有一个实例化时，该依赖名称被分类为其实际解析结果；当多个实例化在种类上一致时，只有它们共享的修饰符保留下来；当它们不一致时（一个将名称解析为函数，另一个解析为变量），该 token 被分类为冲突——实现之间的分歧是信息，而不是噪音。目前单元只记录显式实例化定义产生的实例化；在使用点记录隐式实例化是同一方向的计划扩展。
- 计划：对依赖名称执行 go-to-implementation 将列出它在每个实例化中的解析结果，就像对虚函数执行 go-to-implementation 会列出其 override 一样。这些实现关系如何在符号索引中建模，目前有意留待后续决定。

基于遍历的功能（inlay hints、folding ranges、document symbols）仍然跳过实例化子树：它们发出以位置为键的条目，实例化只能重复——或矛盾于——模式已经产生的内容。这一区分是有意为之：合并多个解析结果对分类和导航有意义，但对逐位置渲染没有意义。

## 已知局限

表达式级推导被有意排除在解析器重写之外——那里的边界情况无穷无尽，解析器的约定是优雅降级（名称未解析时原样返回）而不是猜测。已知的缺口都属于这一类：

- **非类型模板参数表达式**：复合 NTTP 表达式不会被替换或推导（例如，以 `N + 1` 为键选择偏特化）；只有直接的参数值会传递。
- **约束求值**：受约束的偏特化会被检测到，但其约束不会被求值——纯粹由 `requires` 子句选择的特化会降级为未解析，而不是被选中。
- **复杂 `decltype`**：只处理 `decltype(var)` 这种简单情况；成员访问、函数调用和其他复杂表达式不受支持，而用作作用域限定符的依赖 `decltype` 会使整个链条无法解析。
- **深层依赖成员链**：链式成员访问（`box.inner.leaf`）只解析一跳；中间成员的类型不会反馈到下一跳的查找中。`x.template foo<T>()` 成员表达式同样尚未实现。
- **运算符查找**：依赖上下文中的运算符（例如 `a + b`，其中 `a` 的类型依赖模板参数）不由解析器自身解析。独立于解析器，当存在实例化时，模板体内出现的运算符仍可通过实例化信息着色和导航（参见“实例化作为实现”一节）。
