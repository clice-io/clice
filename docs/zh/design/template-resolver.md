# 模板解析器

## 背景

在 C++ 中，模板代码的类型信息要到实例化时才会确定。编译器在解析模板定义时，会将依赖于模板参数的名称标记为"依赖名称（dependent name）"，推迟到实例化阶段再做解析。这是 C++ 两阶段名称查找（two-phase name lookup）的核心规则。

对于语言服务器来说，这意味着模板函数体内的大量名称是不可用的。考虑以下代码：

```cpp
template <typename T>
void process(std::vector<T>& vec) {
    auto it = vec.begin();
    it->  // 这里应该补全什么？
}
```

`it` 的类型是 `std::vector<T>::iterator`，这是一个依赖名称。编译器只知道它依赖于 `T`，但不知道它具体是什么类型。在 AST 中，它被表示为一个 `DependentNameType` 节点，语言服务器看到的只是"某个和 `T` 有关的类型"——无法提供补全、跳转定义、悬停信息等任何语言功能。

问题在简单场景下还可以接受。`vec.push_back()` 这样的调用，语言服务器可以通过查看 `vector` 主模板的成员来提供基本的补全。但实际的 C++ 代码，尤其是涉及标准库的代码，远比这复杂：

```cpp
template <typename T>
void process(std::vector<std::vector<T>>& vec) {
    vec[0].push_back(T{});  // vec[0] 的类型是什么？
}
```

`vec[0]` 返回 `std::vector<std::vector<T>>::reference`。在标准库的实现中，要知道这个类型实际上是 `std::vector<T>&`，需要追踪一条很长的 typedef 链：`reference` → `allocator_traits<Alloc>::value_type` → `__alloc_traits<Alloc>::reference` → ...每一步都涉及偏特化匹配、默认模板参数展开、typedef 解析等操作。

clangd 在 Clang 中实现了一个 `HeuristicResolver` 来处理依赖名称（该类最初在 clangd 内部，后来上游到了 Clang 库中）。它的做法是：遇到依赖名称时，尝试在主模板的定义中查找对应的成员。这个策略能覆盖一部分简单场景，但存在根本性的限制：

- **只查找主模板**。标准库大量使用偏特化来定义成员类型。例如 `allocator_traits` 的成员类型定义在对 `allocator<T>` 的偏特化中，主模板中根本没有这些成员。`HeuristicResolver` 在这些场景下完全失效。

- **不建立参数映射**。即使在主模板中找到了一个 typedef `using value_type = T`，这里的 `T` 是被查找模板的参数，不是调用方的参数。在 `vector<MyType>::value_type` 这样的场景下，需要知道 `T = MyType`，但 `HeuristicResolver` 不做参数推导，无法建立这种映射。

- **不处理默认模板参数**。`std::vector<T>` 实际上是 `std::vector<T, std::allocator<T>>`。第二个参数是默认的，但 `HeuristicResolver` 不展开默认参数，导致依赖于分配器类型的名称无法解析。

clangd 的 issue 中有大量与此相关的用户报告。[#1671](https://github.com/clangd/clangd/issues/1671) 反映了通过 typedef 引用的类型无法解析：用户定义了 `MetaWaldo<T>::Type = Waldo<T>`，对 `Type` 类型的变量调用 `find()` 时，go-to-definition 无法找到目标。[#307](https://github.com/clangd/clangd/issues/307) 反映了依赖基类的成员查找不工作：从 `Base<T>` 继承的成员，在派生类模板中使用时无法跳转。这些问题的共同根源是 `HeuristicResolver` 的解析深度不足——它只做一层浅层查找，不追踪 typedef 链，不匹配偏特化，不穿透基类。

clice 重新设计了依赖名称的解析机制。clice 的 TemplateResolver 实现了一个完整的伪实例化（pseudo-instantiation）引擎，能够在没有具体类型参数的情况下，通过模板参数推导和 typedef 展开，将依赖类型解析为可用的形式。

## 设计

### 伪实例化

TemplateResolver 的核心思想是伪实例化：不需要知道模板参数 `T` 的具体类型，只需要追踪 `T` 在 typedef 链中的传播路径，将最终结果用 `T` 表达出来。

以 `std::vector<std::vector<T>>::reference` 为例。真正的模板实例化需要将 `T` 替换为具体类型（如 `int`），然后逐步展开所有 typedef。伪实例化的做法是：将 `T` 本身作为"已知的"参数，追踪它在标准库 typedef 链中的传播，最终得到 `std::vector<T>&`。这个结果已经足够语言服务器提供补全和类型信息——用户看到的是一个有意义的类型，而不是 `<dependent type>`。

TemplateResolver 对外提供三类操作：

- **类型解析**：接受一个依赖类型，返回解析后的类型。例如将 `typename vector<T>::value_type` 解析为 `T`。
- **名称查找**：在嵌套名称限定符（nested name specifier）中查找声明。例如在 `vector<T>::` 中查找 `iterator`，返回对应的声明。
- **类型重糖化（resugar）**：将 Clang 内部的规范化模板参数类型（用深度+索引表示）映射回带有参数名称的类型，使结果对用户友好。

### 两阶段变换

解析依赖名称时，查找和替换两个操作会相互交织。查找一个 typedef 成员后，需要展开它的底层类型；展开后的类型中可能包含新的依赖名称，需要继续查找。但这种递归关系中隐藏着循环的可能：一个 typedef 的底层类型可能通过若干中间步骤又引用到自身。

TemplateResolver 用两阶段设计来打破这种循环：

**第一阶段**负责启发式查找。它处理各种依赖名称节点——`DependentNameType`（如 `typename Container::value_type`）、`DependentTemplateSpecializationType`（如 `Alloc::rebind<U>`）、`TemplateTypeParmType`（模板参数本身），对它们执行成员查找、偏特化匹配、参数推导等操作。

**第二阶段**只做参数替换和 typedef 展开，不执行任何启发式查找。当第一阶段需要展开一个 typedef 的底层类型时，它委托给第二阶段完成。第二阶段遇到依赖名称时不会尝试查找——只是替换其中已知的模板参数，然后原样返回。

这样做的效果是：typedef 展开永远不会触发新的启发式查找。启发式查找可能产生需要展开的 typedef，但展开过程不会反过来产生新的查找请求。循环链条被切断。

### 实例化栈

模板参数用"深度+索引"来标识——深度对应模板的嵌套层级，索引是同一层中参数的位置。在多层嵌套模板中，不同层级的参数需要同时追踪：

```cpp
template <typename X>
struct Outer {
    template <typename Y>
    struct Inner {
        using type = std::pair<X, Y>;
        // X: 深度 0, 索引 0
        // Y: 深度 1, 索引 0
    };
};
```

TemplateResolver 维护一个实例化栈来管理参数绑定。每当解析器进入一层模板的上下文（通过参数推导确定了该层的参数绑定），就向栈中推入一帧。查找参数时，从栈顶向栈底搜索，匹配深度相同的帧。

当栈为空时（没有外围模板上下文），解析器会沿声明的外围模板上下文向上遍历，将每一层的注入模板参数（injected template arguments）推入栈中，构建出初始上下文。

### 缓存与降级

同一个编译单元中，相同的依赖类型节点可能出现在多个位置。TemplateResolver 在编译单元的生命周期内维护一个解析结果缓存，以 AST 节点指针为键。同一个节点在同一个编译单元中不会被重复解析。

解析过程中的任何步骤都可能失败——查找不到成员、循环检测触发、递归深度超限。TemplateResolver 在所有失败路径上返回原始的依赖类型，不抛异常也不报错。这意味着解析失败的最坏情况等同于不做解析——LSP 功能退化到显示 `<dependent type>`，但不会崩溃或返回错误的结果。

## 实现

### TreeTransform 基础设施

Clang 提供了 TreeTransform 机制：通过继承 `TreeTransform` 基类并覆盖特定类型节点的变换方法，可以递归地变换整个类型树。TemplateResolver 的两个阶段各对应一个 `TreeTransform` 子类——第一阶段的 PseudoInstantiator 覆盖了 `DependentNameType`、`DependentTemplateSpecializationType`、`TemplateTypeParmType`、`TypedefType`、`DecltypeType` 等节点的变换方法；第二阶段的 SubstituteOnly 只覆盖 `TemplateTypeParmType`（参数替换）和 `TypedefType`（typedef 展开），不覆盖 `DependentNameType`（不做查找）。

> 这里的关键约束是：SubstituteOnly 不覆盖 `TransformDependentNameType`。基类的默认行为是替换限定符中的参数后原样重建 `DependentNameType`，不做任何查找。这正是打破循环所需要的行为。

### DependentNameType 的解析流程

以 `typename vector<T>::value_type` 的解析为例，流程如下：

1. 检查缓存，如果命中直接返回
2. 检查是否正在解析同一个节点（循环检测），如果是则返回原始类型
3. 变换限定符部分 `vector<T>`——替换其中的模板参数，必要时递归解析
4. 在变换后的类型中查找 `value_type`：
   - 提取出类模板声明
   - 依次尝试每个偏特化：推导模板参数，如果匹配成功，在偏特化及其基类中查找成员
   - 如果偏特化都没有匹配，尝试主模板及其基类
5. 找到成员声明后，提取其底层类型（如 typedef 的底层类型或 record 类型）
6. 通过第二阶段展开底层类型中的 typedef，用当前栈中的参数绑定进行替换
7. 弹出查找过程中推入的栈帧——替换已经完成，后续处理应该在调用方的上下文中进行
8. 如果替换结果仍包含依赖名称，递归调用第一阶段继续解析
9. 将最终结果写入缓存并返回

整个过程有 16 层的递归深度限制。

### 偏特化匹配

当需要在一个类模板中查找成员时，解析器利用 Clang 的模板参数推导机制来匹配偏特化。具体来说，它将可见的模板参数传给 Clang 的 `DeduceTemplateArguments`，尝试将它们与每个偏特化的模式进行匹配。如果推导成功，就在该偏特化的声明上下文中查找目标成员。

以一个简化的例子说明：

```cpp
template <typename Alloc>
struct alloc_traits;  // 主模板：没有定义 value_type

template <typename T>
struct alloc_traits<allocator<T>> {
    using value_type = T;  // 只在偏特化中定义
};
```

解析 `alloc_traits<allocator<int>>::value_type` 时：

- 在主模板中查找 `value_type`——不存在
- 尝试偏特化 `alloc_traits<allocator<T>>`，推导出 `T = int`——匹配成功
- 在偏特化中找到 `value_type = T`，替换 `T` 为 `int`，返回 `int`

对于符号化的参数（如 `alloc_traits<allocator<U>>`，其中 `U` 是调用方的模板参数），同样的推导机制可以建立 `T = U` 的映射，最终返回 `U`。

### 默认参数处理

在推导模板参数时，如果提供的参数数量少于模板声明的参数数量，解析器会尝试填充默认参数。默认参数本身可能依赖于已提供的参数（如 `vector` 的默认分配器 `allocator<T>` 依赖于 `T`），因此默认参数的展开也需要通过第二阶段的替换来完成。

### 基类查找

如果在类模板自身的成员中没有找到目标名称，解析器会继续在其基类中查找。每个基类类型先通过第二阶段替换模板参数，然后递归进入查找流程。这使得通过继承获得的成员也可以被解析——例如 CRTP 模式中，派生类通过基类访问成员类型。

解析器对同一个（类模板声明, 目标名称）组合进行循环检测，防止自引用继承链（如 CRTP 中 `callback_traits<F> : callback_traits<decltype(&F::operator())>`）导致无限递归。

### 标准库特殊路径

标准库的分配器重绑定链是一个特殊的深层 typedef 链。以 `vector<T>` 为例，查找其 `reference` 成员需要经过：

```text
vector<T, allocator<T>>
  → __alloc_traits<allocator<T>>
    → allocator_traits<allocator<T>>::rebind_alloc<T>
      → allocator<T>::rebind<T>::other  (C++17 前)
      或直接替换第一个模板参数        (C++20 后)
```

这个链条的每一步都涉及 `DependentTemplateSpecializationType` 的解析，深度容易超过限制。解析器对 `std::allocator_traits::rebind_alloc` 这一特定模式进行短路处理：检测到该模式后，先尝试标准的分配器重绑定协议（`Alloc::rebind<T>::other`），如果失败（如 C++20 移除了 `allocator::rebind`），则回退到直接替换分配器的第一个模板参数。

### ResugarOnly

Clang 内部用规范化的 `TemplateTypeParmType` 表示模板参数——它只包含深度和索引（如 "深度 0 的第 0 个参数"），不包含参数名称。这在 Clang 内部是充分的，但用户需要看到 `T`，而不是一个匿名参数。

ResugarOnly 是一个独立的 TreeTransform，它沿着声明的外围模板上下文向上遍历，收集每一层的模板参数列表，然后将规范化的参数类型映射回对应的命名声明。这个变换在 hover 等需要展示类型信息的场景中使用。

## FAQ

- **为什么不直接使用 Clang 的模板实例化机制？**

  Clang 的 `TemplateInstantiator` 需要完整的实参列表才能工作——它要求将每个模板参数替换为具体类型。在模板定义阶段，这些信息不存在。即使人为选择一个占位类型（如 `int`），也会导致错误的结果：`vector<int>::value_type` 是 `int`，但用户需要看到的是 `T`。伪实例化的设计选择是保留符号参数，让结果中的 `T` 保持其原始含义。此外，编辑器中的代码经常是不完整的或有语法错误的，完整的模板实例化在这些场景下会直接失败。

- **TemplateResolver 和 Clang 的 HeuristicResolver 是什么关系？**

  两者解决的是同一类问题，但深度不同。`HeuristicResolver` 做浅层查找——在主模板中直接查找成员名称，不处理偏特化、不做参数推导、不展开 typedef 链。它的优点是简单且不容易出错。TemplateResolver 做深层的伪实例化——追踪参数传播、匹配偏特化、展开 typedef 链，能覆盖更多场景，但复杂度也高得多。目前 clice 在部分功能中（如 hover 的声明目标查找）仍使用 `HeuristicResolver`，因为那部分代码来源于 clangd 的 find_target 逻辑。TemplateResolver 的集成正在逐步推进。

- **为什么不用单阶段设计？**

  考虑这样的场景：类型 `A<T>::type` 是一个 typedef，底层类型是 `B<T>::value_type`，而 `B<T>::value_type` 又是通过另一个 typedef 间接引用了 `A<T>::type`。单阶段设计中，解析 `A<T>::type` 会展开其 typedef，遇到 `B<T>::value_type`，触发查找，查找结果又需要展开，再次遇到 `A<T>::type`——形成循环。两阶段设计中，typedef 展开由第二阶段完成，第二阶段不做查找，因此 `B<T>::value_type` 在展开过程中不会被解析，循环不会发生。代价是第二阶段可能返回不够精确的结果（某些依赖名称没有被解析），但这比无限递归好得多。

- **为什么要对标准库模式做特殊处理？**

  标准库的分配器重绑定链是实际代码中最常遇到的深层 typedef 链。通用的递归解析在处理这个链条时容易超过深度限制，因为每一步都涉及 `DependentTemplateSpecializationType` 的解析和偏特化匹配。特殊处理直接短路了这个常见模式，代价是代码中多了一个 ad-hoc 的分支。未来的方向是改进通用解析能力或引入一个可扩展的模式匹配机制来替代硬编码。

## 已知局限

- **多元素包展开**：目前只处理单元素参数包（常见的包转发场景），不支持多元素包的展开。

  ```cpp
  template <typename... Ts>
  struct Tuple {
      using first = typename std::tuple_element<0, std::tuple<Ts...>>::type;
      // Ts... = {int, float, double} 时无法正确展开
  };
  ```

- **非类型模板参数的默认值**：只处理类型模板参数（`typename T`）的默认值，不处理非类型模板参数（`int N`）和模板模板参数的默认值。

  ```cpp
  template <typename T, int N = 0>
  struct Array {
      using type = T;  // 当 N 有默认值时，参数检查可能失败
  };
  ```

- **依赖成员表达式**：`expr.template member<T>()` 形式的依赖成员访问尚未实现。

  ```cpp
  template <typename T>
  void foo(T& obj) {
      obj.template bar<int>();  // 无法解析 bar 的声明
  }
  ```

- **复杂的 decltype 表达式**：只处理 `decltype(var)` 形式的简单变量引用，不支持成员访问、函数调用等更复杂的表达式。

- **依赖上下文中的运算符**：运算符在 Clang 的 AST 中没有 `IdentifierInfo`，无法通过名称查找来解析。

  ```cpp
  template <typename T>
  auto add(T a, T b) {
      return a + b;  // 如果 T 重载了 operator+，无法解析到该声明
  }
  ```

- **表达式级别的解析**：目前 TemplateResolver 主要处理类型级别的依赖名称（`DependentNameType` 等）。表达式级别的解析（如 `CXXUnresolvedConstructExpr`、`UnresolvedLookupExpr`）已声明接口但尚未实现。此外，TemplateResolver 尚未完全集成到所有 LSP 功能中——部分功能（如 inlay hints 的依赖调用、语义标记的依赖名称着色）仍在等待接入。
