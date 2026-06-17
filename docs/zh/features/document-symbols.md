# 文档符号

通过 `textDocument/documentSymbol` 提供文件大纲和面包屑导航。

## 符号层级

- [x] 嵌套的文档符号树（父子关系）
- [x] 符号范围和选择范围
- [x] UTF-16 位置编码
- [ ] 符号树中的访问修饰符节点 — 将 `public:` / `private:` / `protected:` 显示为分组节点以便面包屑导航（[clangd#499](https://github.com/clangd/clangd/issues/499)）

  ```
  Widget (class)
  ├─ public
  │  ├─ draw() (method)
  │  └─ resize() (method)
  └─ private
     ├─ width (field)
     └─ height (field)
  ```

- [ ] 匿名命名空间 / 无名结构体分组

  ```
  (anonymous namespace) (namespace)
  └─ helper() (function)
  ```

## 符号种类

- [x] 命名空间
- [x] Class / Struct / Union
- [x] Enum / 枚举成员
- [x] 函数 / 方法 / 构造函数
- [x] 变量 / 字段 / 绑定
- [x] 模板声明（通过内部模板化实体）
- [ ] Typedef / 类型别名
- [ ] Concept

## 符号详情

- [ ] `detail` 字段中的函数签名 — 参数类型和名称用于重载区分（[clangd#520](https://github.com/clangd/clangd/issues/520)、[clangd#601](https://github.com/clangd/clangd/issues/601)、[clangd#1232](https://github.com/clangd/clangd/issues/1232)）

  ```
  // 无 detail 时的大纲：
  process (function)        ← 哪个重载？
  process (function)

  // 有 detail 时的大纲：
  process(int x) (function)
  process(std::string s) (function)
  ```

- [ ] `detail` 字段中的变量类型

  ```
  // 大纲："timeout" detail: "int"
  // 大纲："logger" detail: "std::shared_ptr<Logger>"
  ```

- [ ] 类声明的 `detail` 字段中显示基类

  ```
  // 大纲："Circle" detail: ": Shape"
  ```

- [ ] 大纲中的签名去除默认参数值（[clangd#221](https://github.com/clangd/clangd/issues/221)）

  ```
  // 源码：void open(std::string path, int mode = 0644);
  // 大纲：open(string path, int mode) — 无 "= 0644"
  ```

- [ ] 多行函数签名的正确符号范围 — 范围应包含完整签名以使 VS Code sticky scroll 正常工作（[clangd#2221](https://github.com/clangd/clangd/issues/2221)）

  ```cpp
  void Widget::processData(       // ← 符号范围从此处开始
      const Config& cfg,
      int flags
  ) {                              // ← 不是从此处
  ```

## 缺失的符号

- [ ] 大纲中的宏定义（[clangd#1744](https://github.com/clangd/clangd/issues/1744)）

  ```
  MAX_BUFFER_SIZE (macro)
  CHECK(cond, msg) (macro)
  ```

- [ ] 大纲中的 include 指令（[clangd#2226](https://github.com/clangd/clangd/issues/2226)）

  ```
  #include <vector> (include)
  #include "config.h" (include)
  ```

- [ ] 函数体内的局部变量（[clangd#616](https://github.com/clangd/clangd/issues/616)）
- [ ] Module 声明（`module`、`import`、`export module`）

## 符号标签

- [ ] `[[deprecated]]` 符号的 deprecated 标签
- [ ] 访问修饰符指示（public / private / protected）（[clangd#2123](https://github.com/clangd/clangd/issues/2123)）
- [ ] Static / virtual / abstract 指示

## 位置正确性

- [ ] 宏内定义的符号的正确源位置（[clangd#475](https://github.com/clangd/clangd/issues/475)）

  ```cpp
  #define DEFINE_HANDLER(name) void name()
  DEFINE_HANDLER(onReady);  // 大纲应导航到此行，而非宏定义处
  ```

- [ ] 宏定义的变量的选择范围应指向变量名而非宏 token（[clangd#1941](https://github.com/clangd/clangd/issues/1941)）

## 变更记录

| 日期 | 变更                       | PR  |
| ---- | -------------------------- | --- |
| —    | 嵌套符号层级、基本符号种类 | —   |
