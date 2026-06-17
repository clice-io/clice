# 代码补全

## Include 路径补全

由 `<`、`"`、`/` 字符触发。在 AST 之前处理（preamble 层，无需编译）。

**触发上下文**

- [x] `#include <` — 系统/尖括号 include 路径
- [x] `#include "` — 引号 include 路径（优先项目本地）
- [ ] `#include_next` — 需要检测指令是 `#include_next` 而非 `#include`，并将搜索起点调整为提供当前文件的目录之后

  ```cpp
  // 在 <bits/stl_vector.h> 中，由 /usr/include/c++/14/ 提供
  #include_next <^>  // 搜索从 /usr/include/c++/14/ 之后开始，跳过该目录
  ```

- [ ] `__has_include()` / `__has_embed()` — 在这些构造内部触发 include 路径补全

  ```cpp
  #if __has_include(<^>)  // 建议头文件，与 #include < 相同
  ```

- [ ] `#embed` 指令补全

  ```cpp
  #embed <^>  // 建议可嵌入的资源文件
  ```

**候选项与排序**

- [x] 遍历编译数据库中的编译器搜索路径
- [x] 文件和目录都作为候选项，通过结尾是否携带 `/` 来区分目录和文件
- [ ] 过滤已经 include 的头文件

  ```cpp
  #include <vector>
  #include <^>  // 不应再建议 "vector"
  ```

- [ ] 降低私有/内部头文件的排序优先级 — 正常用户不应直接 include 的路径：
  - 单 `_` 前缀：较低优先级（如 `_ctype.h`）
  - 双 `__` 前缀：更低优先级（编译器内置实现如 `__config`、`__bit_reference`）
  - 路径中包含 `detail`、`internal`、`impl`、`bits` 等关键词（第三方库私有头文件如 `boost/detail/`、`bits/stdc++.h`）

  ```cpp
  #include <^>        // __config, _ctype.h, bits/stdc++.h 排到底部
  #include <boost/^>  // boost/detail/ 排序低于 boost/asio/
  ```

- [ ] 基于路径距离排序：项目树中与当前文件距离更近的头文件排序更高

**插入行为**

- [ ] 目录补全不应插入尾部 `/` — 让用户手动输入以重新触发下一级补全（目前 `/` 直接包含在插入文本中，导致编辑器不会自动触发下一轮补全）（[clangd#395](https://github.com/clangd/clangd/issues/395)）

  ```cpp
  #include <sys^>  // 接受 "sys" → 插入 "sys"，用户输入 "/" → 触发下一级补全
  ```

## Module 补全

通过文本上下文分析检测。在 AST 之前处理（preamble 层，无需编译）。

### Import

光标在 `import` 或 `export import` 之后时触发。

- [x] `import` / `export import` — 建议工作区中所有已知模块名
- [x] 前缀过滤（输入时缩小结果范围）
- [x] 通过 `insert_text` 自动追加 `;`
- [x] `CompletionItemKind::Module` 图标
- [ ] 空格触发（[#460](https://github.com/clice-io/clice/pull/460)）

  需要双层门控以避免每次按空格都触发补全：
  1. **服务端**：将 ` `（空格）注册为触发字符，使客户端在空格时发送补全请求。
  2. **扩展端 middleware**：拦截空格触发的请求，仅当当前行匹配 `import ` 或 `export import ` 时转发至服务端（轻量字符串检查，非 import 空格零 IPC 开销），其余情况直接返回空结果。

  此方案与 TypeScript/Haxe 语言扩展使用相同模式（[vscode#67714](https://github.com/microsoft/vscode/issues/67714)）。

- [ ] 从结果中排除自身模块（self-import 无效）— **FIXME**
- [ ] 同一模块内的 partition import

  ```cpp
  // 在 module foo 内部
  import :^  // 建议 :core, :io（仅 foo 自己的 partition）
  ```

  注：`import M:part;` 不是合法的 C++ 语法 — partition 只能在同一模块内通过短形式 `import :part;` 导入。

- [ ] 层级 dot 补全

  ```cpp
  import std.^  // 建议 io, compat 等
  ```

  注：模块名中的点号仅是命名约定，并非语言层面的层级关系，但 dot 触发补全仍然是有价值的用户体验。

- [ ] 过滤其他模块的非导出（内部）partition
- [ ] Header unit import

  ```cpp
  import <^>  // 建议可导入的头文件（与 #include 相同的候选项）
  import "^"  // 同上，引号形式
  ```

- [ ] 符号补全时自动插入 `import` 语句（类似头文件的 auto-include）

  ```cpp
  std::vector^  // 接受补全后，同时在文件顶部插入 "import std;"
  ```

### Declaration

模块声明上下文中的补全（`module` / `export module`）。

- [ ] `import` / `module` 关键字补全

  ```cpp
  imp^  // 建议 "import" 关键字
  mod^  // 建议 "module" 关键字
  ```

- [ ] `module` / `export module` 后的模块名补全

  ```cpp
  module my^  // 建议已有模块名（编写实现单元时有用）
  ```

- [ ] `:` 之后的 partition 名补全

  ```cpp
  export module mylib:^  // 建议 mylib 已有的 partition 名
  module mylib:^  // 同上，用于 partition 实现单元
  ```

- [ ] `module :private;` 补全（private module fragment）

  ```cpp
  module :^  // 建议 "private"
  ```

- [ ] 主接口单元中 `export import :partition` 的 re-export 补全

  ```cpp
  // 在 mylib 的主接口单元中
  export import :^  // 建议 mylib 需要 re-export 的接口 partition
  ```

## 语义代码补全

由 `.`、`->`、`::` 或 quickSuggestions 触发。通过 stateless worker 转发给 Clang `CodeCompleteConsumer`。

### 成员访问

- [x] `.` — struct/class 成员
- [x] `->` — 指针成员访问（通过 Clang fixup）
- [x] `::` — 命名空间/类作用域成员
- [ ] Dot-to-arrow：在指针上输入 `.` 时自动触发 `->` 成员补全并替换（[clangd#1349](https://github.com/clangd/clangd/issues/1349)）

  ```cpp
  std::unique_ptr<Foo> ptr;
  ptr.^  // 建议 Foo 的成员，插入为 ptr->bar()
  ```

- [ ] 将第一个参数类型匹配的自由函数与成员结果一起显示

  ```cpp
  std::vector<int> v;
  v.^  // 同时建议 std::sort(v, ...)、std::find(v, ...) 等
  ```

- [ ] 在成员建议中显示 `operator[]`、`operator->`、`operator()`
- [ ] 优先显示所输入操作符的直接成员（`.` 时优先 `.` 成员，`->` 时优先 `->` 成员）

### Designated Initializer（指定初始化器）

- [ ] 按声明顺序排序补全结果（C++20 designated initializer 要求字段按声明顺序出现）（[clangd#965](https://github.com/clangd/clangd/issues/965)）

  ```cpp
  struct Cfg { int width; int height; bool fullscreen; };
  Cfg c = { .^  // 建议：.width, .height, .fullscreen（按此顺序）
  ```

- [ ] 过滤已使用的 designator

  ```cpp
  Cfg c = { .width = 800, .^  // 仅建议 .height, .fullscreen
  ```

- [ ] 复合字面量 designated initializer（`(struct T){ .field = }`）
- [ ] 匿名 struct/union 成员 designator

  ```cpp
  struct S { union { int i; float f; }; };
  S s = { .^  // 建议 .i, .f
  ```

- [ ] "填充所有成员" snippet

  ```cpp
  Cfg c = { ^  // 第一项：.width = ${1}, .height = ${2}, .fullscreen = ${3}
  ```

### Override 与类外定义

- [ ] 虚函数 override 补全，带完整签名和 `override` 关键字

  ```cpp
  struct Base { virtual void draw(int x, int y) const; };
  struct Derived : Base {
      ^  // 建议：void draw(int x, int y) const override
  };
  ```

- [ ] 完整继承层次遍历以获取 override 候选（[clangd#226](https://github.com/clangd/clangd/issues/226)、[clangd#2374](https://github.com/clangd/clangd/issues/2374)）

  ```cpp
  struct A { virtual void f(); };
  struct B : A { };
  struct C : B {
      ^  // 建议：void f() override（从 A 经由 B 继承）
  };
  ```

- [ ] 类外定义补全

  ```cpp
  // 在 .cpp 文件中
  void MyClass::^  // 建议所有成员函数，带完整签名 + 函数体 snippet
  ```

- [ ] 在定义上下文中显示所有成员（包括 private/protected）

  ```cpp
  class Foo { private: void secret(); };
  void Foo::^  // 必须包含 "secret" — 这是定义，不是调用
  ```

- [ ] 定义上下文中 `::` 后显示构造函数
- [ ] 类模板的构造函数/析构函数省略冗余模板参数

  ```cpp
  template<typename T>
  struct Vec { Vec(); ~Vec(); };

  template<typename T>
  Vec<T>::^  // 建议 "Vec()" 和 "~Vec()"，而非 "Vec<T>()" 或 "~Vec<T>()"
  ```

### 符号

- [x] 非限定名查找（局部变量、函数、类型）
- [x] 限定名查找（`std::`）
- [x] 参数相关查找（ADL）候选
- [x] 关键字补全（if、for、while 等）
- [x] 宏补全
- [ ] 带占位符的 Snippet 模式（函数体、控制流）
- [ ] C++ attribute 补全

  ```cpp
  [[^]]  // 建议：nodiscard, deprecated, maybe_unused, likely, ...
  ```

- [ ] 跨作用域补全，包括 class/struct 作用域的符号（内部类型、静态方法）

  ```cpp
  struct Outer { struct Inner {}; static int count; };
  Inn^  // 从不同作用域建议 Outer::Inner
  ```

- [ ] 插入限定符时尊重命名空间别名（优先使用最短有效限定符）

  ```cpp
  namespace fs = std::filesystem;
  fs::ex^  // 插入 "fs::exists"，而非 "std::filesystem::exists"
  ```

- [ ] 语言感知过滤（混合 C/C++ 项目中 C 文件不出现 C++ 符号）
- [ ] 函数参数注释补全（`/*param=*/` 风格的参数提示）
- [ ] 语义分析不可用时基于标识符的回退补全

### 函数与 Snippet

- [x] 函数重载分组（`bundle_overloads`，默认：开）
- [ ] 参数占位符 snippet（`enable_function_arguments_snippet`，默认：关 — LSP 路径尚未接入）
- [x] 签名在 `label_details.detail` 中，返回类型在 `label_details.description` 中
- [ ] 模板参数占位符（`enable_template_arguments_snippet`）
- [ ] 自动插入括号（`insert_paren_in_function_call`）
- [ ] 前瞻检测已有括号/方括号，避免重复插入

  ```cpp
  foo^(10, 20);  // 不应再插入一对括号 → foo(10, 20)
  ```

- [ ] 上下文感知 snippet：函数指针上下文中只插入函数名（不加调用语法）

  ```cpp
  void (*fp)(int) = my_fun^;  // 插入 "my_func"，而非 "my_func(${1:int x})"
  ```

- [ ] 剥离 C++23 显式对象参数（explicit object parameter）

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // 显示签名 "(int x)"，而非 "(this S& self, int x)"
  ```

- [ ] 在签名中显示默认参数值（[clangd#100](https://github.com/clangd/clangd/issues/100)）

  ```cpp
  void open(std::string path, int mode = 0644);
  open(^  // detail 显示 "(string path, int mode = 0644)"
  ```

- [ ] 将 lambda 类型解析为实际签名

  ```cpp
  auto cmp = [](int a, int b) -> bool { return a < b; };
  cmp^  // 显示 "(int a, int b) -> bool"，而非 "<lambda>"
  ```

- [ ] 解析转发函数的参数（[clangd#447](https://github.com/clangd/clangd/issues/447)）

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(^  // 显示 "(int w, int h)"
  ```

- [ ] `InsertReplaceEdit` 支持（同时提供 insert 和 replace 范围，用于单词中间的补全）

  ```cpp
  refact^orize  // insert: "refactoring^orize"，replace: "refactoring"
  ```

- [ ] 无占位符时设置 `InsertTextFormat::PlainText`

### 模板与 Concept

- [ ] Concept 感知补全：从模板参数的 concept 约束中推断可用成员（[clangd#1103](https://github.com/clangd/clangd/issues/1103)）

  ```cpp
  template<typename T>
  concept Drawable = requires(T t) { t.draw(); t.resize(int{}, int{}); };

  template<Drawable T>
  void render(T& widget) {
      widget.^  // 从 Drawable concept 推断建议 draw(), resize()
  }
  ```

- [ ] 未实例化模板中的依赖类型成员补全

  ```cpp
  template<typename T>
  void process(std::vector<std::vector<T>>& matrix) {
      matrix[0].^  // 解析 operator[] → vector<T>&，建议 push_back(), size() 等
  }
  ```

- [ ] 利用单一实例化信息进行 generic lambda 补全 — 当 generic lambda 仅从一个调用点调用时，使用该调用点的参数类型在 lambda 体内提供补全

  ```cpp
  std::vector<std::string> names;
  std::ranges::sort(names, [](const auto& a, const auto& b) {
      return a.^  // a 可从唯一调用点推断为 std::string
  });
  ```

  ```cpp
  auto results = names | std::views::transform([](const auto& s) {
      return s.^  // s 可推断为 std::string
  });
  ```

- [ ] 类模板体内部的注入类名（injected class name）不加模板参数 snippet

  ```cpp
  template<typename T>
  struct Vec {
      Vec^  // 建议 "Vec"，而非 "Vec<${1:T}>"
  };
  ```

### 宏

- [x] 从 AST 补全宏名
- [x] 宏使用与其他符号相同的模糊匹配器
- [ ] 正确的 `CompletionItemKind`：function-like 宏为 `Function`，object-like 宏为 `Constant`（目前全部为 `Unit`）（[clangd#2002](https://github.com/clangd/clangd/issues/2002)）
- [ ] 将宏定义/展开显示为文档（[clangd#1485](https://github.com/clangd/clangd/issues/1485)）

  ```cpp
  #define MAX_BUF 4096
  MAX^  // 补全详情显示：#define MAX_BUF 4096
  ```

- [ ] function-like 宏的参数占位符（尊重 snippet 设置）

  ```cpp
  #define CHECK(cond, msg) ...
  CHECK^  // 插入：CHECK(${1:cond}, ${2:msg})
  ```

- [ ] 宏参数内部的补全，回退到外围上下文

  ```cpp
  #define WRAP(...) __VA_ARGS__
  WRAP(some_obj.^)  // 仍应提供 some_obj 的成员
  ```

### 过滤与排序

- [x] 具有词边界感知评分的模糊匹配（camelCase、snake_case）
- [x] 模糊过滤与前缀匹配
- [x] 过滤恢复上下文结果（`CCC_Recovery`）
- [x] 过滤 `_` 前缀的内部符号（除非用户输入了 `_`）
- [x] 已弃用符号标记
- [ ] 结果数量限制（`CodeCompletionOptions.limit`）
- [ ] 最近使用/频率提升
- [ ] 将数字-字母边界视为分词点（[clangd#1236](https://github.com/clangd/clangd/issues/1236)）

  ```cpp
  i32^  // 应匹配 int32_t（数字-字母边界："32" → "t"）
  ```

- [ ] 作用域感知的相关性分层：局部变量 > 成员 > 命名空间作用域 > 跨作用域
- [ ] 基于上下文类型提升（期望类型为枚举时提升匹配的枚举成员）（[clangd#462](https://github.com/clangd/clangd/issues/462)）

  ```cpp
  enum Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // 将 Red, Green, Blue 提升到顶部
  ```

- [ ] 在 switch 语句中过滤已使用的枚举值

  ```cpp
  switch (color) {
      case Red: break;
      case ^  // 仅建议 Green, Blue — Red 已使用
  ```

- [ ] C++ 模式下 `nullptr` 排在 `NULL` 前面
- [ ] 命名信号提升

  ```cpp
  auto foo = get^;  // 提升 getFoo() 高于 getBar()
  ```

- [ ] 引用计数与文件距离排序信号
- [ ] 机器学习排序模型

## 自动 Include 插入

尚未实现。补全一个符号时不会插入 `#include` 指令。

- [ ] 接受补全时为未解析的符号插入 `#include`

  ```cpp
  std::vec^  // 接受 "vector" 后，同时在文件顶部插入 #include <vector>
  ```

- [ ] 检查传递性 include 图以避免重复 include

  ```cpp
  // <algorithm> 已经传递性地 include 了 <iterator>
  std::back_inserter^  // 不应再插入 #include <iterator>
  ```

- [ ] 上下文感知：前向声明或仅指针/引用用法不插入 include（[clangd#639](https://github.com/clangd/clangd/issues/639)）

  ```cpp
  class Foo;
  Foo*^  // 不需要 include — 前向声明对指针就够了
  ```

- [ ] C 文件插入 C 头文件，C++ 文件插入 C++ 头文件

  ```c
  // 在 .c 文件中
  size_^  // 插入 #include <stddef.h>，而非 #include <cstddef>
  ```

- [ ] 可配置行为：`always` / `iwyu-only` / `never`
- [ ] 优先使用项目相对路径而非绝对路径
- [ ] 尊重 IWYU pragma 和头文件映射
- [ ] 为 C++20 模块符号自动插入 `import`

## 补全项中的文档

尚未实现。补全项不包含文档信息。

- [ ] 从声明和定义中提取文档注释

  ```cpp
  /// @brief 在指定路径打开文件。
  /// @param path 文件系统路径。
  void open(std::string path);

  op^  // 补全弹窗显示 @brief 文档
  ```

- [ ] 无论定义位于何处（头文件、源文件、索引）都可用
- [ ] 将模板模式的文档传播到实例化
- [ ] 标准库文档集成

## 触发字符

已注册：`. < > : " / *`。空格（` `）已计划但尚未合并（[#460](https://github.com/clice-io/clice/pull/460)）。

| 字符 | 上下文        | 行为                                                                                     |
| ---- | ------------- | ---------------------------------------------------------------------------------------- |
| `.`  | 成员访问      | 语义补全                                                                                 |
| `->` | 指针成员      | `[ ]` 尚未工作 — dot-to-arrow fix-it 未传播                                              |
| `::` | 通过 `:` 触发 | 作用域补全                                                                               |
| `<`  | `#include <`  | Include 路径补全                                                                         |
| `>`  | 模板关闭      | 语义补全                                                                                 |
| `"`  | `#include "`  | Include 路径补全                                                                         |
| `/`  | 路径分隔符    | Include 路径续补                                                                         |
| `*`  | 指针解引用    | 语义补全                                                                                 |
| ` `  | `import` 之后 | Module 名补全（扩展门控）— **待合并 [#460](https://github.com/clice-io/clice/pull/460)** |

## LSP 协议特性

- [ ] `completionItem/resolve` 延迟加载文档和详情
- [ ] `CompletionList.isIncomplete` 标志用于增量过滤
- [ ] `commitCharacters` 在特定按键时自动接受补全
- [ ] `filterText` / `sortText` 用于客户端侧重新过滤

## 变更记录

| 日期 | 变更                           | PR  |
| ---- | ------------------------------ | --- |
| —    | 初始 include/语义补全          | —   |
| —    | Module import 补全（扁平前缀） | —   |
