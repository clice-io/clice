# 诊断

## 核心

- [x] Clang 诊断（错误、警告、备注）
- [x] 严重程度映射（错误、警告）
- [x] 带源位置的诊断范围
- [x] 相关信息（附加到诊断的备注）
- [x] 跨文件诊断的文件 URI 转换
- [ ] 拉取式诊断模型（`textDocument/diagnostic`）（[clangd#2108](https://github.com/clangd/clangd/issues/2108)）
- [ ] 报告所有因缺少 `#include` 而产生的错误，而不只是第一个——解析器遇到首个致命错误后便会停止

  ```cpp
  #include "missing_a.h"  // error reported
  #include "missing_b.h"  // error NOT reported (parser already stopped)
  #include "missing_c.h"  // error NOT reported
  ```

- [ ] 显示源自头文件的诊断的完整包含链（[clangd#1392](https://github.com/clangd/clangd/issues/1392)）

  ```
  // current:  "In included file: expected ';'"
  // expected: "In main.cpp → utils.h → detail/impl.h: expected ';'"
  ```

- [ ] 在诊断中反映头文件的未保存更改（[clangd#488](https://github.com/clangd/clangd/issues/488)）

  ```
  // header.h (unsaved buffer): added new_func()
  // main.cpp: calls new_func() → should NOT show "undeclared identifier"
  ```

- [ ] Preamble 头文件中的模板实例化错误诊断（[clangd#137](https://github.com/clangd/clangd/issues/137)）

## 标签

- [x] `-Wdeprecated` 诊断的 `Deprecated` 标签
- [x] 未使用变量/参数警告的 `Unnecessary` 标签

## 发布

- [x] 编译完成时推送诊断
- [x] 文件关闭时清除诊断
- [x] 按文件对诊断分组（主文件和头文件）
- [x] 诊断 `code` 字段包含 Clang 错误代码
- [ ] `codeDescription` 中提供指向 Clang 文档的链接
- [x] 诊断 `source` 字段区分 clang 与 clang-tidy
- [ ] 可配置计算诊断前的防抖延迟（[clangd#1471](https://github.com/clangd/clangd/issues/1471)）
- [ ] 后台索引完成后重新计算已打开文件的诊断（[clangd#2604](https://github.com/clangd/clangd/issues/2604)）

## 诊断抑制

- [x] 使用 `// NOLINT` 注释抑制诊断
- [x] 使用 `// NOLINTNEXTLINE` 注释抑制诊断
- [x] `// NOLINTBEGIN` / `// NOLINTEND` 块级抑制
- [ ] include-cleaner 诊断支持 `NOLINT`（[clangd#1982](https://github.com/clangd/clangd/issues/1982)）
- [ ] 配置文件中可按诊断类别配置严重程度（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 根据版本控制 diff 过滤诊断——仅显示变更行附近的警告（[clangd#822](https://github.com/clangd/clangd/issues/822)）

## 诊断操作

- [ ] 将自动修复建议作为代码操作附加到诊断

## 头文件诊断

- [ ] 针对未使用或缺少的 `#include` 指令提供 include-cleaner 诊断
- [ ] 抑制头文件中的 static inline 函数所触发的 `-Wunused-function` 误报（[clangd#1211](https://github.com/clangd/clangd/issues/1211)）

  ```cpp
  // utils.h
  static inline int helper() { return 42; }
  // should NOT warn "unused function" when checking header standalone
  ```

- [ ] 传递来自头文件的 `-Wpadded` 等布局警告（[clangd#1429](https://github.com/clangd/clangd/issues/1429)）
- [ ] 抑制 Preamble 优化导致的 `-Wempty-translation-unit` 误报（[clangd#2358](https://github.com/clangd/clangd/issues/2358)）
- [ ] 跨头文件边界的线程安全分析（[clangd#2386](https://github.com/clangd/clangd/issues/2386)）

## clang-tidy 集成

- [ ] clang-tidy 诊断（由配置控制）
- [x] 抑制源自系统头文件中宏的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] Clang 静态分析器支持（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 特定版本的 clang-tidy 文档链接（[clangd#2136](https://github.com/clangd/clangd/issues/2136)）
- [ ] 对位于代码之前的预处理指令进行诊断（[clangd#2501](https://github.com/clangd/clangd/issues/2501)）

## 诊断显示

- [ ] 限定类型名的正确诊断范围——为完整名称标注下划线，而不只是基名（[clangd#1035](https://github.com/clangd/clangd/issues/1035)）

  ```cpp
  ns::Inner obj(42);
  //  ^^^^^ only base name underlined
  // should underline "ns::Inner"
  ```

- [ ] 将优化备注（`-Rpass`）显示为诊断（[clangd#2519](https://github.com/clangd/clangd/issues/2519)）
- [ ] 列出整个项目的警告（[clangd#1973](https://github.com/clangd/clangd/issues/1973)）

## 配置文件诊断

- [ ] 为格式错误的 `.clang-tidy` 和 `.clang-format` 文件报告诊断（[clangd#2313](https://github.com/clangd/clangd/issues/2313)、[clangd#2591](https://github.com/clangd/clangd/issues/2591)）

## 诊断正确性

应正确处理的已知问题：

- [ ] 宏重定义警告指向同一位置（[clangd#2479](https://github.com/clangd/clangd/issues/2479)）
- [ ] 针对嵌套数组初始化给出错误的 `-Wmissing-braces` 修复建议（[clangd#2434](https://github.com/clangd/clangd/issues/2434)）
- [ ] Preamble 失效导致诊断消息混杂时出现无效的严重级别 0（[clangd#2124](https://github.com/clangd/clangd/issues/2124)）
- [ ] 未声明标识符诊断被与更正相关的诊断隐藏（[clangd#547](https://github.com/clangd/clangd/issues/547)）
- [ ] `--include` 文件缺失时产生误导性的后续诊断（[clangd#2229](https://github.com/clangd/clangd/issues/2229)）
