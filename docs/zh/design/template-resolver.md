# Template Resolver

## 问题

在 C++ 中，如果一个类型依赖于模板参数，那么在实例化之前无法解析其成员。对于语言服务器来说，这意味着在模板代码内部会丧失功能——没有补全、没有悬停、没有导航。

考虑：

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.  // 光标位置
}
```

clangd 通过假设主模板来处理这种情况，为 `std::vector<T>` 提供补全。但考虑更复杂的情况：

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].  // 光标位置
}
```

`vec2[0]` 的类型是 `std::vector<std::vector<T>>::reference`，这是一个[依赖名称](https://en.cppreference.com/w/cpp/language/dependent_name)。在 libstdc++ 中，解析它需要追踪几十层嵌套的模板 typedef。clangd 在这里失败，因为：

1. 它基于主模板假设，不考虑偏特化可能会使查找无法进行下去
2. 它只进行名称查找而不进行模板实例化，就算找到了最后的结果，也没法把它和最初的模板参数映射起来
3. 不考虑默认模板参数，无法处理由默认模板参数导致的依赖名

## 伪实例化

clice 实现了一个**伪实例化器**（`src/semantic/resolver.cpp`），它在没有具体类型参数的前提下解析依赖类型。通过跟踪 typedef 链并对模板参数进行符号化替换来简化依赖名称。

例如：`std::vector<std::vector<T>>::reference` 被简化为 `std::vector<T>&`，从而支持完整的代码补全。

### 架构

解析器使用 Clang 的 `TreeTransform` 基础设施，分两个阶段：

1. **PseudoInstantiator** — 执行启发式查找和替换。处理：
   - `DependentNameType`（如 `typename Container::value_type`）
   - `DependentTemplateSpecializationType`
   - `TemplateTypeParmType`（将参数映射回其约束）
   - 通过嵌套模板的 typedef 链
   - 默认模板参数

2. **SubstituteOnly** — 循环打破回退。如果实例化器检测到循环（类型 A 依赖类型 B，类型 B 又依赖类型 A），则切换到浅层替换模式以避免无限递归。

### 能力

- 解析深层嵌套的依赖 typedef（处理 libstdc++ 的多层间接引用）
- 跨多个层级追踪模板参数映射
- 处理 `UnresolvedLookupExpr`、`UnresolvedUsingType`、`CXXUnresolvedConstructExpr`
- 通过 `DenseMap<const void*, QualType>` 按 TU 缓存结果以提升性能
- 提供 `resugar()` 以展示用户友好的类型（如 `string` 而不是 `basic_string<char, ...>`）

### 用户可见影响

有了模板解析器，clice 在模板函数体内也能提供代码补全、悬停信息和类型推导——即使是 clangd 无法处理的复杂标准库类型。
