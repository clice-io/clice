# 模板解析

## 背景

C++ 模板的核心设计是**延迟实例化**——模板代码在被具体类型参数实例化之前，编译器不会（也无法）解析依赖于模板参数的名称。这些名称被称为**依赖名称（dependent names）**，它们的类型在模板定义阶段是未知的。

对于语言服务器来说，这意味着模板代码内部是一个"盲区"：

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.  // 光标在这里——vec 的类型已知是 vector<T>，可以提供补全
}
```

对于简单的情况，假设使用主模板的定义即可提供基本的补全。但考虑更复杂的场景：

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].  // vec2[0] 的类型是什么？
}
```

`vec2[0]` 的类型是 `std::vector<std::vector<T>>::reference`——一个依赖名称。在标准库实现中，解析它需要追踪数十层嵌套的模板 typedef 链：`reference` → `allocator_traits<Alloc>::value_type` → `__alloc_traits<Alloc>::reference` → ... 每一层都涉及偏特化匹配、默认模板参数、typedef 展开等复杂操作。

在 clangd 的社区中，模板代码的补全和悬停问题长期存在。用户反复报告在模板函数体内无法获得补全建议——光标处显示 `<dependent type>`，所有 LSP 功能失效。问题的根源在于 clangd 的解析策略存在几个根本性限制：

**不处理偏特化**：clangd 假设总是使用主模板的定义来查找成员。但标准库大量使用偏特化（如 `allocator_traits` 对不同分配器的特化），主模板可能根本没有要查找的成员——成员只存在于特定的偏特化中。

**缺少参数映射**：即使通过名称查找找到了成员的类型，这个类型仍然是用被查找模板的参数表达的（如 `allocator_traits<Alloc>::value_type` 中的 `Alloc`），而不是用调用方的参数（如 `T`）。clangd 不做实例化，因此无法建立参数之间的映射关系。

**忽略默认模板参数**：`std::vector<T>` 实际上是 `std::vector<T, std::allocator<T>>`——第二个参数是默认的。clangd 不展开默认参数，导致依赖于默认参数的名称无法解析。

## 设计方案

### 核心思想

clice 实现了一个**伪实例化器（PseudoInstantiator）**——它在没有具体类型参数的情况下，通过启发式方法解析依赖名称。核心思路是：不需要知道 `T` 是 `int` 还是 `string`，只需要追踪 `T` 在模板 typedef 链中的传播路径，将最终结果用 `T` 表达出来。

例如：`std::vector<std::vector<T>>::reference` 经过伪实例化后被简化为 `std::vector<T>&`——这是一个具体到足以提供补全的类型，同时保留了模板参数 `T` 的符号含义。

### 两阶段变换

解析器基于 Clang 的 TreeTransform 基础设施构建，分为两个阶段：

**第一阶段：PseudoInstantiator（启发式解析）**

这是主引擎，负责解析各种依赖名称：

- **DependentNameType**（如 `typename Container::value_type`）：在模板的成员中查找 `value_type`，匹配偏特化，展开 typedef，将结果用调用方的参数重新表达
- **DependentTemplateSpecializationType**（如 `Alloc::rebind<U>`）：解析依赖的模板特化，会优先尝试标准库特殊路径
- **TemplateTypeParmType**（如 `T`）：通过实例化栈查找参数的绑定值或默认参数
- **DecltypeType**（如 `decltype(var)`）：对简单变量引用解析其声明类型

**第二阶段：SubstituteOnly（循环打破）**

当第一阶段在展开 typedef 时，可能再次遇到需要启发式查找的依赖名称，形成循环：typedef A 的底层类型引用了依赖名称 B，查找 B 发现它的类型又涉及 typedef A。

SubstituteOnly 打破这种循环——它只做参数替换和 typedef 展开，不执行启发式查找。当第一阶段需要展开 typedef 时，委托给 SubstituteOnly 完成，确保不会触发递归的启发式查找。

### 实例化栈

模板参数在嵌套模板中可能处于不同的深度层级。实例化栈（InstantiationStack）维护一个参数映射栈，每一帧记录一层模板的参数绑定。

当遇到 `TemplateTypeParmType`（一个用深度和索引标识的模板参数）时，解析器从栈顶（最内层）到栈底（最外层）线性搜索，匹配对应深度的参数绑定。如果找到，替换为绑定的类型；如果未找到，尝试使用参数的默认值。当栈为空时（独立的依赖类型），解析器会沿着外围模板声明向上走，推入它们的注入模板参数作为上下文。

这使得解析器能够处理多层嵌套模板：

```cpp
template<typename X>
struct Outer {
    template<typename Y>
    struct Inner {
        typename std::pair<X, Y>::first_type member;
        // X 在深度 0，Y 在深度 1
        // 两者都需要在栈中追踪才能正确解析
    };
};
```

### 依赖名称的解析流程

以 `typename A<T>::type` 为例：

1. **缓存检查**：如果该节点已解析过，直接返回缓存结果
2. **循环检测**：如果该节点正在解析中，中止以防止无限递归
3. **限定符变换**：变换 `A<T>` 部分——替换参数、匹配偏特化
4. **成员查找**：在变换后的类型中查找 `type`。依次尝试每个偏特化及其基类，然后是主模板及其基类
5. **参数推导**：使用 Clang 的模板参数推导机制，建立形参到实参的映射
6. **替换**：通过 SubstituteOnly 展开找到的成员类型中的 typedef，用调用方的参数替换
7. **递归**：如果结果仍包含依赖名称，递归解析

整个过程有递归深度限制（16 层），防止极端情况下的无限递归。

解析器还维护了两层循环检测：一层防止对同一个 DependentNameType 节点的重入解析，另一层防止在同一个 ClassTemplateDecl 上的递归查找（处理 CRTP 等自引用模式）。

### 偏特化匹配

偏特化匹配是伪实例化器相对于 clangd 的关键优势。标准库大量使用偏特化，例如：

```cpp
// 主模板——没有定义 value_type
template<typename Alloc> struct allocator_traits;

// 偏特化——定义了 value_type
template<typename T> struct allocator_traits<allocator<T>> {
    using value_type = T;
};
```

当解析 `allocator_traits<allocator<int>>::value_type` 时，clangd 在主模板中查找 `value_type` 失败。伪实例化器会尝试将实际参数与每个偏特化的模式进行匹配，找到正确的偏特化后在其中查找成员。

### 标准库特殊处理

标准库的分配器重绑定链是一个特别深的 typedef 链：

```
vector<T> → __alloc_traits<allocator<T>> → allocator_traits<allocator<T>>
  → allocator_traits<Alloc>::rebind_alloc<U>
```

这个链条可能超过深度限制。解析器对 `allocator_traits::rebind_alloc` 模式进行特殊短路处理：检测到该模式后，直接尝试 `Alloc::rebind<T>::other`（标准分配器协议），或回退到替换第一个模板参数。

### 类型重糖化

Clang 内部用规范化的 `TemplateTypeParmType`（深度+索引）表示模板参数。用户看到的应该是参数名（如 `T`），而不是 `parameter 0 of depth 0`。ResugarOnly 变换将规范化的参数类型映射回其原始声明，用于对用户展示友好的类型信息。

### 优雅降级

解析过程中的任何步骤失败（查找失败、循环检测触发、深度超限），解析器返回原始的依赖类型，而不是报错。这意味着 LSP 功能不会因为解析失败而完全中断——只是退化到"无法解析"的状态，与不做伪实例化的效果相同。

## 设计决策与权衡

**为什么是伪实例化而不是真正的模板实例化？** 真正的实例化需要具体的类型参数（如 `T = int`），但在模板定义阶段没有这些信息。而且语言服务器中的代码经常是不完整的，无法进行完整的实例化。伪实例化通过保留符号参数（`T` 本身），避免了对具体类型的依赖。

**为什么需要两阶段设计？** 单阶段设计中，typedef 展开会触发新的启发式查找，而查找结果可能又需要 typedef 展开——形成循环。两阶段设计通过职责分离打破循环：启发式查找只在第一阶段执行，typedef 展开委托给不做查找的第二阶段。

**为什么要特殊处理标准库模式？** 标准库的分配器重绑定是实际代码中最常见的深层 typedef 链。通用的递归解析在这里会超过深度限制。特殊处理虽然不优雅，但覆盖了最高频的用户场景。未来计划用通用机制替代这些特殊处理。

**为什么选择优雅降级而不是报错？** 模板解析本质上是启发式的——不可能覆盖所有 C++ 模板的边界情况。优雅降级确保解析失败不会比"不做解析"更差，同时在能解析的情况下提供显著的体验提升。

## 已知限制

- **包展开（pack expansion）**：目前只处理单元素包（如包转发），不支持多元素包（如 `Us... = {int, float}` 的展开）。
- **非类型模板参数的默认值**：只支持类型模板参数（TemplateTypeParmDecl）的默认值替换，不支持非类型参数（如 `template<int N = 0>`）和模板模板参数的默认值。
- **依赖成员表达式**：`x.template foo<T>()` 形式的依赖成员访问尚未实现。
- **复杂的 decltype**：只处理 `decltype(var)` 的简单情况，不支持成员访问、函数调用等复杂表达式。
- **运算符查找**：依赖上下文中的运算符（如 `a + b`，其中 `a` 类型依赖于模板参数）无法解析，因为运算符缺少标识符信息。
