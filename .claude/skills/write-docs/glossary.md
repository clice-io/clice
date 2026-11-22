# The wording of clice's Chinese pages

clice's zh pages follow the shared rules of clice-io/docs (RULES.md, the write-docs skill); this file adds clice's page conventions and terms. `pixi run docs-review` hands it to the model, so it speaks to a translator.

## By position on the page

| Where                                       | Rule                                                                                                                                                                                                                                   |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Sidebar groups, nav bar                     | Group labels are translated (指南 / 语言服务器 / 命令行 / 设计). Product names `clice`, `catter`, `kotatsu`, `blog` stay lowercase English; the zh nav calls the blog 博客.                                                            |
| Page and section headings (h1–h3)           | Translated on every page including design/. Headings that are identifiers stay verbatim: `[workers]`, `[[rules]]`, `textDocument/hover`, `clice lint`.                                                                                 |
| Capability card name, summary, description  | Translated, including names that start with code (`auto` deduction → `auto` 推导, Doxygen `///` comments → Doxygen `///` 注释). The one-sentence summary reads as one sentence on its own and ends without a period, like the English. |
| Table headers                               | Option → 选项, Type → 类型, Default → 默认值, Status → 状态.                                                                                                                                                                           |
| Status words                                | 支持 / 部分支持 / 不支持, and the overview's counts in them (支持 N 项 · 部分支持 N 项). A status table row and the card it points to use identical wording.                                                                           |
| Theme UI strings (查看示例, 悬停提示, 页脚) | Not in the pages; the site components switch on `lang`.                                                                                                                                                                                |
| `case` fences                               | Never translated: the fence names the case whose card the site draws.                                                                                                                                                                  |

## By term

Translate:

- Feature names are the overview page's: 代码补全, 悬停, 签名帮助, 代码导航, 重命名, 诊断, 代码操作, 语义 Token, 内联提示, 文档链接, 文档符号, 折叠范围, 选择范围, 格式化, C++20 模块, 头文件上下文, 编译, 调用层次结构, 类型层次结构, 大纲, 工作区符号; Lint stays Lint. LSP request names stay as code when quoted (`textDocument/hover`); the feature is named in Chinese.
- C++ concepts with an established Chinese term: 结构化绑定, 范围 for 循环, 概念, 模板特化, 显式实例化, 折叠表达式, 参数包, 注入类名. On first use in a page, the English follows in full-width parentheses when it is what one would search for: 结构化绑定（structured bindings）, 最令人烦恼的解析（most vexing parse）.
- Common nouns: translation unit → 翻译单元, compilation database → 编译数据库, header → 头文件, index → 索引, snapshot → 快照 (test snapshots and dependency snapshots alike), overload set → 重载集, crash → 崩溃, build → 构建, worker → worker (kept), language server → 语言服务器, project root → 项目根目录, settings → 设置, build configuration → 构建配置, compilation context → 编译上下文, self-contained header → 自包含头文件.

Keep English (never transliterate):

- Product and tool names: VS Code, Neovim, Zed, CMake, Bazel, clang, clang-format, clangd, GCC, MSVC, LLVM, Clang.
- Acronyms: LSP, AST, PCH, PCM, CDB, TU, ADL, CTAD, DAG, ABI, URI, C++23.
- Terms Chinese C++ developers use untranslated: Lambda, Token, Concept (as the language feature; 概念 in prose is fine), `this`, Preamble, fixture.
