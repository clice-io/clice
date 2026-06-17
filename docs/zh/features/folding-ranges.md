# 折叠范围

## 折叠类型

- [x] 块折叠 — 函数、类、结构体、联合体、枚举、命名空间、复合语句、lambda
- [x] 多行列表折叠 — 函数参数、调用参数、初始化列表、lambda 捕获列表

  ```cpp
  void configure(
      int width,           // ┐
      int height,          // │ 可折叠参数列表
      bool fullscreen      // ┘
  );

  auto result = compute(
      getWidth(),          // ┐
      getHeight(),         // │ 可折叠参数列表
      true                 // ┘
  );
  ```

- [x] 访问修饰符区域折叠 — 类内的 `public:` / `protected:` / `private:` 区域（[clangd#1455](https://github.com/clangd/clangd/issues/1455)）

  ```cpp
  class Widget {
  public:            // ┐
      void draw();   // │ 可折叠
      void resize(); // ┘
  private:           // ┐
      int width;     // │ 可折叠
      int height;    // ┘
  };
  ```

- [x] 预处理条件折叠（`#if` / `#ifdef` / `#ifndef` ... `#endif`）（[clangd#1661](https://github.com/clangd/clangd/issues/1661)、[clangd#2059](https://github.com/clangd/clangd/issues/2059)）
- [x] 自定义区域折叠（`#region` / `#endregion`、`#pragma region` / `#pragma endregion`）（[clangd#1623](https://github.com/clangd/clangd/issues/1623)）
- [ ] 注释折叠 — 多行 `/* */` 和连续 `//` 行注释

  ```cpp
  // 这是一段很长的
  // 多行注释
  // 应折叠为一个区域

  /*
   * 块注释
   * 也应可折叠
   */
  ```

- [ ] Include 区域折叠 — 连续的 `#include` 指令

  ```cpp
  #include <vector>       // ┐
  #include <string>       // │ 可折叠区域
  #include <algorithm>    // ┘

  #include "app.h"        // ┐ 单独区域
  #include "config.h"     // ┘（空行分隔）
  ```

- [ ] 原始字符串字面量折叠

  ```cpp
  auto sql = R"(
      SELECT *
      FROM users
      WHERE active = true
  )";  // 可折叠的多行原始字符串
  ```

- [ ] `using` 声明块 — 连续的 using 声明/指令

  ```cpp
  using std::vector;  // ┐
  using std::string;  // │ 可折叠
  using std::map;     // ┘
  ```

- [ ] 模板参数列表折叠

  ```cpp
  template<
      typename Key,            // ┐
      typename Value,          // │ 可折叠
      typename Compare = less  // ┘
  >
  class SortedMap { };
  ```

## 改进

- [ ] `collapsedText` 折叠占位文本（LSP 3.17）— 折叠后显示摘要（[clangd#2667](https://github.com/clangd/clangd/issues/2667)）

  ```
  void processData(const Config& cfg) {...}   // 显示签名 + {...}
  #include <vector>  ... (5 more)             // 显示数量
  /* License header... */                      // 显示首行
  ```

  > **客户端支持**：VS Code **不支持** `collapsedText`（[vscode#70794](https://github.com/microsoft/vscode/issues/70794) — 仍为 open）；Neovim 的 nvim-lsp 原生支持。不支持此字段的客户端会静默忽略 — 折叠仍然有效，只是缺少占位文本。

- [ ] 从声明行开始折叠函数/类体 — 折叠后保留签名可见（[clangd#2666](https://github.com/clangd/clangd/issues/2666)）

  ```cpp
  // 折叠后：void processData(const Config& cfg) {...}
  // 而非：  {...（签名被隐藏在折叠上方）}
  ```

  > **客户端支持**：取决于客户端对 `FoldingRange.startLine` 的解读。VS Code 将 `startLine` 的下一行作为首个隐藏行，因此将 `startLine` 设为声明行即可达到预期效果。但 VS Code 折叠后仍会将 `}` 单独留在下一行，而非收到签名行（[vscode#3352](https://github.com/microsoft/vscode/issues/3352) — 仍为 open）。其他客户端可能不同。

- [ ] 非活跃预处理分支指示 — 视觉区分或自动折叠非活跃的 `#if`/`#else` 分支

  ```cpp
  #ifdef _WIN32
      // ... Windows 代码（活跃）...
  #else
      // ... POSIX 代码（非活跃，可自动折叠）...
  #endif
  ```

  > **备注**：与 semantic tokens（非活跃代码灰显）有交叉，部分属于客户端 UX 范畴。服务器可用 `FoldingRangeKind.Region` 标记这些范围，由客户端决定是否自动折叠。

## 变更记录

| 日期 | 变更                                             | PR  |
| ---- | ------------------------------------------------ | --- |
| —    | 块折叠、列表折叠、访问修饰符折叠、预处理区域折叠 | —   |
