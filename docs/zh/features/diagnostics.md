# 诊断

## 核心

- [x] Clang 诊断（错误、警告、备注）
- [x] 严重性映射（Error、Warning）
- [x] 带源位置的诊断范围
- [x] 关联信息（附加到诊断的备注）
- [x] 跨文件诊断的文件 URI 转换
- [ ] 拉取诊断模型（`textDocument/diagnostic`）（[clangd#2108](https://github.com/clangd/clangd/issues/2108)）
- [ ] 报告所有缺失的 `#include` 错误，而非仅第一个 — 解析器在首个致命错误后停止

  ```cpp
  #include "missing_a.h"  // 报告错误
  #include "missing_b.h"  // 未报告（解析器已停止）
  #include "missing_c.h"  // 未报告
  ```

- [ ] 为源自头文件的诊断显示完整的 include 链（[clangd#1392](https://github.com/clangd/clangd/issues/1392)）

  ```
  // 当前："In included file: expected ';'"
  // 期望："In main.cpp → utils.h → detail/impl.h: expected ';'"
  ```

- [ ] 在诊断中反映头文件的未保存更改（[clangd#488](https://github.com/clangd/clangd/issues/488)）

  ```
  // header.h（未保存缓冲区）：添加了 new_func()
  // main.cpp：调用 new_func() → 不应显示 "undeclared identifier"
  ```

- [ ] 预编译头中模板实例化错误的诊断（[clangd#137](https://github.com/clangd/clangd/issues/137)）

## 标签

- [x] `-Wdeprecated` 诊断的 `Deprecated` 标签
- [x] 未使用变量/参数警告的 `Unnecessary` 标签

## 发布

- [x] 编译完成时推送诊断
- [x] 文件关闭时清除诊断
- [x] 按文件分组诊断（关注的文件 + 头文件）
- [ ] 可配置的诊断计算防抖延迟（[clangd#1471](https://github.com/clangd/clangd/issues/1471)）
- [ ] 后台索引完成后重新计算已打开文件的诊断（[clangd#2604](https://github.com/clangd/clangd/issues/2604)）

## 诊断抑制

- [ ] `// NOLINT` 注释抑制
- [ ] `// NOLINTNEXTLINE` 注释抑制
- [ ] `// NOLINTBEGIN` / `// NOLINTEND` 块抑制
- [ ] include-cleaner 诊断的 `NOLINT` 支持（[clangd#1982](https://github.com/clangd/clangd/issues/1982)）
- [ ] 在配置文件中按诊断类别配置严重性（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 按版本控制 diff 过滤诊断 — 仅显示变更行附近的警告（[clangd#822](https://github.com/clangd/clangd/issues/822)）

## 头文件诊断

- [ ] 抑制头文件中 static inline 函数的误报 `-Wunused-function`（[clangd#1211](https://github.com/clangd/clangd/issues/1211)）

  ```cpp
  // utils.h
  static inline int helper() { return 42; }
  // 单独检查头文件时不应警告 "unused function"
  ```

- [ ] 从头文件传播 `-Wpadded` 等布局警告（[clangd#1429](https://github.com/clangd/clangd/issues/1429)）
- [ ] 抑制预编译头优化导致的误报 `-Wempty-translation-unit`（[clangd#2358](https://github.com/clangd/clangd/issues/2358)）
- [ ] 跨头文件边界的线程安全分析（[clangd#2386](https://github.com/clangd/clangd/issues/2386)）

## clang-tidy 集成

- [ ] clang-tidy 诊断（由配置控制）
- [ ] 抑制源自系统头文件宏的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] Clang 静态分析器支持（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 版本特定的 clang-tidy 文档链接（[clangd#2136](https://github.com/clangd/clangd/issues/2136)）
- [ ] 代码前预处理指令的诊断（[clangd#2501](https://github.com/clangd/clangd/issues/2501)）

## 诊断显示

- [ ] 限定类型名的正确诊断范围 — 下划线标注完整名称，而非仅基本名称（[clangd#1035](https://github.com/clangd/clangd/issues/1035)）

  ```cpp
  ns::Inner obj(42);
  //  ^^^^^ 仅基本名称被标注
  // 应标注 "ns::Inner"
  ```

- [ ] 将优化备注（`-Rpass`）显示为诊断（[clangd#2519](https://github.com/clangd/clangd/issues/2519)）
- [ ] 项目级警告列表（[clangd#1973](https://github.com/clangd/clangd/issues/1973)）

## 配置文件诊断

- [ ] 为格式错误的 `.clang-tidy` 和 `.clang-format` 文件报告诊断（[clangd#2313](https://github.com/clangd/clangd/issues/2313)、[clangd#2591](https://github.com/clangd/clangd/issues/2591)）

## 诊断正确性

应正确处理的已知问题：

- [ ] 宏重定义警告指向同一位置（[clangd#2479](https://github.com/clangd/clangd/issues/2479)）
- [ ] 嵌套数组初始化的错误 `-Wmissing-braces` 修复建议（[clangd#2434](https://github.com/clangd/clangd/issues/2434)）
- [ ] 预编译头失效时混合诊断消息产生无效严重性 0（[clangd#2124](https://github.com/clangd/clangd/issues/2124)）
- [ ] 未声明标识符诊断被纠正相关诊断隐藏（[clangd#547](https://github.com/clangd/clangd/issues/547)）
- [ ] `--include` 文件缺失时产生误导性的下游诊断（[clangd#2229](https://github.com/clangd/clangd/issues/2229)）

## 变更记录

| 日期 | 变更 | PR |
| ---- | ---- | -- |
| —    | Clang 诊断、严重性映射、标签、推送发布 | — |
