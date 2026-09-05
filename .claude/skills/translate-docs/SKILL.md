---
name: translate-docs
description: Rules for the Chinese (docs/zh) tree — what is translated and what stays verbatim, by position on the page and by term. Read BEFORE translating, reviewing, or editing any docs/zh page or the translation prompts in tools/docs/translate.ts.
---

# Translating the docs into Chinese

The zh tree reads as Chinese technical writing, not as glossed English. The
reader is a C++ developer who searches the web in English, so the rule is:
translate the prose, keep the names people search for, and never touch
anything a tool or a compiler reads. The mechanics (segment isomorphism,
`report`/`record`/`check`, the review mode) are in the docs skill; this
skill is only about the words.

## By position on the page

| Where                                          | Rule                                                                                                                                                                                                                                                    |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Sidebar groups, nav bar                        | Group labels are translated (指南 / 语言服务器 / 命令行 / 设计 / 开发). Product names `clice`, `catter`, `kotatsu`, `blog` stay lowercase English; the zh nav calls the blog 博客.                                                                      |
| Page and section headings (h1–h3)              | Prose headings are translated, on every page including design/. A heading that is an identifier stays verbatim: `[project]`, `[[rules]]`, `textDocument/hover`, `clice lint`. A mixed heading translates only the prose part and keeps every code span. |
| Capability card name, summary, description     | Translated, including names that start with code (`auto` deduction → `auto` 推导, Doxygen `///` comments → Doxygen `///` 注释). The one-sentence summary ends without a period, like the English.                                                       |
| Table headers and cells                        | Translated (选项 / 类型 / 默认值 / 状态). Code spans inside cells stay.                                                                                                                                                                                 |
| Status words                                   | Fixed vocabulary only: 支持 / 部分支持 / 不支持 / 已实现 / 存根 / 计划中. The card sticker and the status table must use the same word.                                                                                                                 |
| Task-list items                                | Text translated, `[ ]` / `[x]` state kept.                                                                                                                                                                                                              |
| Theme UI strings (查看示例, 悬停提示, 页脚)    | Not in the pages; the site components switch on `lang`.                                                                                                                                                                                                 |
| Code blocks, `snap` fences, `// snap:` blocks  | Never translated, byte-identical to English. **Comments inside code blocks stay English too** — they are part of the fixture, and a translated comment would no longer match the source the site renders.                                               |
| Inline code, paths, config keys, command lines | Never translated; the checker compares them literally.                                                                                                                                                                                                  |
| Issue references, URLs, link targets           | Verbatim.                                                                                                                                                                                                                                               |
| Compiler and tool output quoted in prose       | Verbatim.                                                                                                                                                                                                                                               |

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
  header → 头文件, index → 索引, crash → 崩溃, build → 构建, worker → worker
  (kept), language server → 语言服务器.

Keep English (never transliterate):

- Product and tool names: VS Code, Neovim, Zed, CMake, Bazel, clang,
  clang-format, clangd, GCC, MSVC, LLVM, Clang.
- Acronyms: LSP, AST, PCH, PCM, CDB, TU, ADL, CTAD, DAG, ABI, URI, C++23.
- Terms Chinese C++ developers use untranslated: Lambda, Token, Concept
  (as the language feature; 概念 in prose is fine), `this`, Preamble,
  Overload set, fixture, snapshot. When in doubt, keep the English term and
  add a short Chinese gloss rather than invent a translation.

## Style

- Full-width punctuation inside Chinese sentences; a space between CJK and
  Latin text (prettier enforces it).
- No machine-translation calques: no "这个" as an article, no passive
  chains, no stacked 进行 / 对于 / 通过……的方式; split long relative clauses.
- Banned filler: 深入, 强大, 无缝, 赋能, 极大地, 显著地, 值得注意的是,
  总而言之, 综上所述.
- Say what the English says, not word for word. A card summary must read as
  one sentence on its own.
- One term, one translation within a page; a status table row and the card
  it points to use identical wording.

## Where the rules live in code

`tools/docs/translate.ts` embeds these rules in `REVIEW_PROMPT`. Change the
prompt and this skill together.
