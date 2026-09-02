---
name: docs
description: The clice documentation system — generated feature/config pages, the en↔zh translation contract, and the pixi commands driving them. Read BEFORE editing anything under docs/.
---

# Documentation system

## Layout and sources of truth

- `docs/en/` — English pages. Handwritten pages (design/, guide/, dev/,
  index.md) are edited directly; feature pages (features/\*.md) contain
  GENERATED regions rendered from snapshot fixtures, and
  guide/configuration.md is rendered from the config schema. **Never edit
  inside a `<!-- BEGIN GENERATED ... -->` region** — edit the fixture doc
  header (see the write-tests skill) or the config annotations instead.
- `docs/zh/` — Chinese pages, equally real and equally hand-edited (by a
  person or a model). Each zh page must stay **segment-isomorphic** to its
  en counterpart: same sequence of markdown blocks, translated text in the
  translatable blocks, code blocks and HTML comments byte-identical.
  `check`, `report` and `record` never write these files; only an
  explicit `translate <page>` overwrites the named zh page.
- `docs/meta/translations/` — one JSON per page pair: an ordered list of
  `{kind, en-hash, zh-hash}` pairs, each attesting "these two segments were
  last reviewed as translations of each other". Maintained exclusively by
  `record`; never edit by hand.
- Each tree has its own hand-maintained `sidebar.yaml`.
- `docs/public/clice-config.schema.json` — committed output of
  `clice inspect --config-schema`; CI checks freshness.

## Commands (pixi)

| command                            | what it does                                       |
| ---------------------------------- | -------------------------------------------------- |
| `pixi run check-feature-docs`      | feature pages match their fixtures (CI)            |
| `pixi run update-feature-docs`     | rewrite feature GENERATED regions                  |
| `pixi run check-config-docs`       | configuration page matches the schema (CI)         |
| `pixi run update-config-docs`      | rewrite the configuration page                     |
| `pixi run check-doc-translations`  | hard gate: zh isomorphic to en, all pairs attested |
| `pixi run report-doc-translations` | translator worklist: drifted segments with texts   |
| `pixi run record-doc-translations` | re-attest hash pairs after deliberate edits        |
| `pixi run review-doc-translations` | model review of zh pages, segment by segment       |

## Translation contract (tools/docs/translate.ts)

Pages split into segments: headings, paragraphs, blockquotes, list items,
table rows, and index.md's YAML frontmatter are translatable; everything
else (code blocks, HTML comments including GENERATED markers) is verbatim
and must be byte-identical across the two trees, as must any fenced code
or HTML comment nested inside a translatable segment (a snap example
under a generated capability's paragraph). Segment shapes must match too: heading depth,
ordered vs. bulleted list, task-list state, table column count and
alignment, and the mapping/sequence skeleton of index.md's frontmatter. No text is stored twice — the mapping holds
hashes only. Old wording of a drifted segment comes from git history of
the markdown page.

Workflow for any edit touching translated pages:

1. Edit the en page (or zh — the contract is symmetric: polishing one side
   requires re-reviewing the other).
2. `pixi run report-doc-translations` — lists every broken pair with the
   current en and zh texts side by side.
3. Update the counterpart page so both sides correspond again.
4. `pixi run format` first, then `pixi run record-doc-translations` —
   the formatter canonicalizes markdown (table padding, emphasis style,
   CJK spacing) and changes segment hashes, so recording before it means
   re-recording after. Record rewrites the mapping; the diff of the JSON
   shows exactly which pairs were re-attested. Never run record without
   having reviewed what report showed: record blesses whatever is on
   disk.
5. Commit markdown + mapping together; `check` must be green.

This applies to generated regions too: after `update-feature-docs` changes
an en feature page, the zh page must receive the translated equivalent in
the same PR — batched at the end of the branch, see below.

Machine drafting: `DEEPSEEK_API_KEY=... node tools/docs/translate.ts
translate [page...]` produces isomorphic zh drafts via the DeepSeek API
(no args = only pages missing a zh counterpart; explicit pages overwrite,
feeding the current zh text to the model as terminology reference).
Fenced code inside segments is masked out of the round trip and restored
byte-for-byte. A segment the model cannot render validly is left in
English and the run exits non-zero naming the page — rerun `translate`
on it after review. The key comes from the environment and is never
stored. Drafts still go through review and `record`.

## Chinese wording: what is translated and what stays English

The zh tree reads as Chinese technical writing, not as glossed English.
The reader is a C++ developer who searches the web in English, so the
rule is: translate the prose, keep the names people search for.

Translate:

- Page, section and capability titles, table headers and cells, list
  items, descriptions. Feature names have fixed Chinese names — use the
  ones the overview page uses (代码补全, 悬停, 签名帮助, 代码导航,
  文档链接, 语义 Token, 内联提示, 折叠范围, 文档符号, 格式化, 诊断,
  代码操作; Lint stays Lint). LSP request names stay as code when
  quoted (`textDocument/hover`), the feature is named in Chinese.
- C++ concepts that have an established Chinese term: 结构化绑定, 范围
  for 循环, 概念, 模板特化, 显式实例化, 折叠表达式, 参数包, 注入类名.
  On the first use in a page, give the English in full-width
  parentheses when the English is what one would search for: 结构化绑定
  （structured bindings）, 最令人烦恼的解析（most vexing parse）.
- Status words: 支持 / 部分支持 / 不支持.

Keep English (never transliterate):

- Product and tool names: VS Code, Neovim, Zed, CMake, Bazel, clang,
  clang-format, clangd, GCC, MSVC, LLVM.
- Acronyms: LSP, AST, PCH, PCM, CDB, TU, ADL, CTAD, DAG, ABI, URI, C++23.
- Anything in code font: identifiers, keywords, file paths, config keys
  and TOML sections, command lines, diagnostics text quoted from the
  compiler. Option values are code too (`"off"`, `"on"`) and keep their
  backticks.
- Terms that are commonly used untranslated by Chinese C++ developers
  and whose translations are less recognizable: Lambda, Token, Concept
  when naming the language feature (概念 in prose is fine), `this`,
  Preamble, Overload set. When in doubt, keep the English term and add
  a short Chinese gloss rather than invent a translation.

Style: full-width punctuation inside Chinese sentences, a space between
CJK and Latin text (prettier enforces it), no machine-translation
calques ("这个" for "the", passive-voice chains), sentences that say
what the English says rather than word for word.

Reviewing existing Chinese pages: `pixi run review-doc-translations
[page...]` (default: every page) feeds each translatable segment with
its current Chinese to a model and writes the corrected Chinese back,
one chunk of segments per call, code blocks masked out — the model
never sees a code block, and a reply that breaks a segment's shape keeps
the current text. The default backend is the codex CLI run from an empty
scratch directory (`--jobs=N` parallel calls, `--effort=LEVEL`);
`--backend=deepseek` uses the API. Review the diff, then `format` and
`record`. Prefer this over handing a model whole pages: the code blocks
would only burn its context.

## Syncing docs at the end of a branch

Generated regions and translations are synced **once per branch, right
before the pre-push checks** of the pr skill — not after every fixture or
page edit, and not in the main conversation: delegate it to a subagent so
the report output and page texts never enter the main context. Give the
subagent this skill and `git diff --name-only origin/main...HEAD`; its
brief is:

1. If snap fixtures with doc headers or config annotations changed:
   `pixi run update-feature-docs` and `pixi run update-config-docs`
   rewrite the en GENERATED regions.
2. `pixi run report-doc-translations` lists every broken pair with both
   texts. Translate each new or drifted en segment into the zh page,
   keeping the skeleton (same block kind, list marker, heading depth,
   nested code byte-identical) and the terminology of the surrounding
   page; delete zh segments whose en segment is gone. For whole new
   pages, or dozens of drifted pages, the `translate` mode below drafts
   them when a DeepSeek key is available — otherwise translate by hand.
3. `pixi run format`, then `pixi run record-doc-translations`, then
   `pixi run check-doc-translations`, `check-feature-docs` and
   `check-config-docs` — all green.
4. Report back: pages touched, how many segments were translated, and
   anything deliberately left as is.

**docs/ contains no changelog content at all** — neither per-page
"Changelog" sections nor standalone changelog pages. Both were removed
deliberately (2026-09) as redundant maintenance burden; do not reintroduce
them. Feature history lives in git/PRs; LLVM upgrade notes live in the
upgrade-llvm skill's `llvm-changelog.md`. For future deliberately
untranslated pages, the tool has an `UNTRANSLATED_PREFIXES` hook
(currently empty): listed pages need no zh counterpart and no mapping.

## What belongs in a "Known Limitations" section

Design-level, user-visible trade-offs that are stable on a months timescale,
written in behavior terms — they answer the reader's "why does it work this
way". Bugs never go there: they live in the internal bug inventory and simply
disappear when fixed; putting them in docs creates staleness debt. Feature
coverage gaps are already expressed by the generated status tables. The flow is
one-way: an internal item graduates into a doc limitation only once it is
decided to be design (or long-term deferral), and a doc limitation is removed
only when the design changes. Never reference internal IDs, file paths, or
timelines in docs.

The contract went live 2026-09-02: all pages machine-drafted
(deepseek-v4-pro), recorded, `check` green, and the gate wired into the
CI docs check. The legacy hand-written zh tree it replaced survives in
git history.
