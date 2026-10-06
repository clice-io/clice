---
name: translate-docs
description: clice's glossary and page conventions for the Chinese (docs/zh) tree, on top of the shared translation rules of clice-io/docs. Read BEFORE translating, reviewing, or editing any docs/zh page.
---

# Translating the clice docs into Chinese

clice is a C++ language server; its zh docs follow the shared rules of
clice-io/docs, **RULES.md at v1** — the contract, what stays verbatim by
position on the page, and style:

    gh api 'repos/clice-io/docs/contents/tools/translations/RULES.md?ref=v1' -H 'Accept: application/vnd.github.raw'

Read it first. This page adds only what is clice's: the page conventions
of its site and its glossary. `pixi run review-doc-translations` hands
this page to the model as the glossary, so it speaks to a translator.
The mechanics (`report`/`record`/`check`, syncing at the end of a branch)
are in the docs skill.

## By position on the page

| Where                                       | Rule                                                                                                                                                                                                                                   |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Sidebar groups, nav bar                     | Group labels are translated (指南 / 语言服务器 / 命令行 / 设计 / 开发). Product names `clice`, `catter`, `kotatsu`, `blog` stay lowercase English; the zh nav calls the blog 博客.                                                     |
| Page and section headings (h1–h3)           | Translated on every page including design/. Headings that are identifiers stay verbatim: `[project]`, `[[rules]]`, `textDocument/hover`, `clice lint`.                                                                                 |
| Capability card name, summary, description  | Translated, including names that start with code (`auto` deduction → `auto` 推导, Doxygen `///` comments → Doxygen `///` 注释). The one-sentence summary reads as one sentence on its own and ends without a period, like the English. |
| Table headers                               | Option → 选项, Type → 类型, Default → 默认值, Status → 状态.                                                                                                                                                                           |
| Status words                                | Fixed vocabulary only: 支持 / 部分支持 / 不支持 / 已实现 / 存根 / 计划中. The card sticker and the status table must use the same word, and a status table row and the card it points to use identical wording.                        |
| Theme UI strings (查看示例, 悬停提示, 页脚) | Not in the pages; the site components switch on `lang`.                                                                                                                                                                                |
| `snap` fences, `// snap:` blocks            | Code: never translated, comments included — they are part of the fixture, and a translated comment would no longer match the source the site renders.                                                                                  |

## By term

Translate:

- Feature names have fixed Chinese names — use the ones the overview page
  uses: 代码补全, 悬停, 签名帮助, 代码导航, 文档链接, 语义 Token, 内联提示,
  折叠范围, 文档符号, 格式化, 诊断, 代码操作; Lint stays Lint. LSP request
  names stay as code when quoted (`textDocument/hover`); the feature is
  named in Chinese.
- C++ concepts with an established Chinese term: 结构化绑定, 范围 for 循环,
  概念, 模板特化, 显式实例化, 折叠表达式, 参数包, 注入类名. On first use in
  a page, give the English in full-width parentheses when the English is
  what one would search for: 结构化绑定（structured bindings）,
  最令人烦恼的解析（most vexing parse）.
- Common nouns: translation unit → 翻译单元, compilation database → 编译数据库,
  header → 头文件, index → 索引, snapshot → 快照 (test snapshots and
  dependency snapshots alike), overload set → 重载集, crash → 崩溃,
  build → 构建, worker → worker (kept), language server → 语言服务器.

Keep English (never transliterate):

- Product and tool names: VS Code, Neovim, Zed, CMake, Bazel, clang,
  clang-format, clangd, GCC, MSVC, LLVM, Clang.
- Acronyms: LSP, AST, PCH, PCM, CDB, TU, ADL, CTAD, DAG, ABI, URI, C++23.
- Terms Chinese C++ developers use untranslated: Lambda, Token, Concept
  (as the language feature; 概念 in prose is fine), `this`, Preamble,
  fixture.
