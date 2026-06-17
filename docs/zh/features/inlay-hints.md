# Inlay Hints

## 参数名提示

- [x] 调用点处的命名参数提示
- [x] 参数名与实参名一致时跳过
- [x] 语义明确的单参数调用跳过
- [x] 展开参数包（`underlying_pack_type` 检测）
- [x] 从定义（不仅是声明）解析参数名
- [ ] 转发函数参数解析 — 对 `std::make_unique`、`emplace_back` 等显示底层构造函数参数（[clangd#2324](https://github.com/clangd/clangd/issues/2324)）

  ```cpp
  struct Widget { Widget(int width, int height); };
  auto p = std::make_unique<Widget>(800, 600);
  //                                ^^^  ^^^
  //                          width: 800, height: 600（而非 __args: 800, __args: 600）
  ```

- [ ] 函数指针和 `operator()` 参数提示（[clangd#1734](https://github.com/clangd/clangd/issues/1734)、[clangd#1742](https://github.com/clangd/clangd/issues/1742)）

  ```cpp
  void (*callback)(int status, const char* msg);
  callback(0, "ok");
  //       ^   ^^
  //  status: 0, msg: "ok"

  auto cmp = [](int a, int b) { return a < b; };
  cmp(1, 2);
  //  ^  ^
  //  a: 1, b: 2
  ```

- [ ] 模板参数提示 — 显示推导/显式的模板参数（[clangd#2583](https://github.com/clangd/clangd/issues/2583)）

  ```cpp
  template<typename T, typename U>
  auto convert(U val) -> T;
  convert<float>(42);
  //      ^^^^^  ^^
  //   T: float, val: 42
  ```

- [ ] 大小写不敏感的参数名匹配 — `aParam` 应在实参为 `param` 时抑制提示（[clangd#2248](https://github.com/clangd/clangd/issues/2248)）

- [ ] 当行内注释已标注参数名时抑制提示（[clangd#1877](https://github.com/clangd/clangd/issues/1877)）

  ```cpp
  draw(/*x=*/10, /*y=*/20);  // 无需提示 — 行内注释已起到相同作用
  ```

## 类型提示

- [x] `auto` 推导类型提示
- [x] Structured binding 类型提示
- [x] Lambda 返回类型提示
- [x] Range-based for 循环变量类型提示
- [ ] Lambda init-capture 类型提示（[clangd#1163](https://github.com/clangd/clangd/issues/1163)）

  ```cpp
  auto f = [val = compute()] {};
  //        ^^^ : int
  ```

- [ ] 依赖 `auto` 类型提示 — 即使在模板体内也显示有意义的推导类型（[clangd#2275](https://github.com/clangd/clangd/issues/2275)）

- [ ] 推导失败时不显示类型提示（[clangd#1475](https://github.com/clangd/clangd/issues/1475)）

- [ ] 类型已显式指定时不显示类型提示（[clangd#1749](https://github.com/clangd/clangd/issues/1749)）

  ```cpp
  auto x = static_cast<int>(val);  // 无需类型提示 — 已是显式的
  auto y = int{42};                 // 同上
  ```

- [ ] 优先显示脱糖类型 — 显示 `std::vector<int>` 而非 typedef 别名（[clangd#1298](https://github.com/clangd/clangd/issues/1298)、[clangd#1668](https://github.com/clangd/clangd/issues/1668)）

  ```cpp
  using IntVec = std::vector<int>;
  IntVec create();
  auto v = create();
  //   ^ : std::vector<int>（而非 IntVec）
  ```

- [ ] 可配置的类型提示长度限制（[clangd#1357](https://github.com/clangd/clangd/issues/1357)）

- [ ] 缩写类型提示与可展开标签部件 — 使用 LSP `InlayHintLabelPart` 允许展开截断的类型（[clangd#2269](https://github.com/clangd/clangd/issues/2269)）

  ```
  auto it = map.begin();
  //   ^^ : map<str…, int>::iterator  [点击展开]
  ```

- [ ] 抑制明显的作用域以缩短提示（[clangd#2270](https://github.com/clangd/clangd/issues/2270)）

  ```cpp
  // 在 namespace foo 内部：
  auto x = create();
  //   ^ : Bar（而非 foo::Bar）
  ```

## Designator 提示

- [ ] 聚合初始化 designator（`.field =`）— 对位置式聚合初始化显示字段名

  ```cpp
  struct Point { int x, y, z; };
  Point p = {1, 2, 3};
  //         ^  ^  ^
  //    .x = 1, .y = 2, .z = 3
  ```

- [ ] 括号聚合初始化（C++20）（[clangd#2540](https://github.com/clangd/clangd/issues/2540)）

  ```cpp
  Point p(1, 2, 3);
  //      ^  ^  ^
  // .x = 1, .y = 2, .z = 3
  ```

- [ ] 紧凑的数组 designator 格式（[clangd#2303](https://github.com/clangd/clangd/issues/2303)）

  ```cpp
  int arr[] = {10, 20, 30};
  //           ^^  ^^  ^^
  //      [0]= 10, [1]= 20, [2]= 30
  ```

## 隐式转换提示

- [ ] 在调用点和赋值处显示隐式类型转换（[clangd#2254](https://github.com/clangd/clangd/issues/2254)）

  ```cpp
  void process(double val);
  process(42);
  //      ^^
  //      (double) 42  — 隐式 int→double 转换

  std::string s = "hello";
  //              ^^^^^^^
  //    (std::string) "hello"  — 隐式 const char*→string 转换
  ```

## 引用 / 指针提示

- [ ] 显示 `&` / `&&` 以指示实参通过可变引用传递（[clangd#1123](https://github.com/clangd/clangd/issues/1123)）

  ```cpp
  void sort(std::vector<int>& v);
  sort(data);
  //   ^^^^
  //   &data  — 通过可变引用传递
  ```

## 模板参数提示

- [ ] CTAD 推导的模板参数（[clangd#2331](https://github.com/clangd/clangd/issues/2331)）

  ```cpp
  std::pair p(1, 3.14);
  //        ^ <int, double>
  ```

## 块结尾提示

- [ ] 长代码块关闭花括号后显示声明名（[clangd#1634](https://github.com/clangd/clangd/issues/1634)）

  ```cpp
  void Widget::processData(const Config& cfg) {
      // ... 50+ 行 ...
  } // Widget::processData
  ```

- [ ] `#endif` 提示 — 显示对应的宏条件（[clangd#2487](https://github.com/clangd/clangd/issues/2487)）

  ```cpp
  #ifdef _WIN32
      // ...
  #endif // _WIN32
  ```

## 交互功能

- [x] 范围作用域查询（仅计算可见范围内的提示）
- [x] 左锚定提示（参数名在实参前）
- [x] 右锚定提示（类型在变量名后）
- [ ] 可点击的类型名 — 通过 LSP `InlayHintLabelPart` 在提示中的类型名上 go-to-definition（[clangd#1535](https://github.com/clangd/clangd/issues/1535)）

  ```
  auto widget = create();
  //   ^^^^^^ : Widget  ← 点击 "Widget" 跳转到 Widget 的定义
  ```

## 提示正确性

应正确处理的已知问题：

- [ ] 嵌套宏调用不应产生错误的参数名提示（[clangd#2620](https://github.com/clangd/clangd/issues/2620)）
- [ ] 显式函数模板实例化时不应产生重复提示（[clangd#1034](https://github.com/clangd/clangd/issues/1034)）
- [ ] 从派生类调用继承构造函数时应显示参数提示（[clangd#1364](https://github.com/clangd/clangd/issues/1364)）
- [ ] 协程返回模板类型时不应丢失参数提示（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）
- [ ] C++23 deducing `this` — 提示显示中去除显式对象参数（[clangd#1777](https://github.com/clangd/clangd/issues/1777)）

## 变更记录

| 日期 | 变更                                 | PR  |
| ---- | ------------------------------------ | --- |
| —    | 参数名提示、类型提示、范围作用域查询 | —   |
