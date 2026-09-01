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
  translatable blocks, code blocks and HTML comments byte-identical. The
  tool never writes these files.
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

## Translation contract (tools/docs/translate.ts)

Pages split into segments: headings, paragraphs, blockquotes, list items,
table rows, and index.md's YAML frontmatter are translatable; everything
else (code blocks, HTML comments including GENERATED markers) is verbatim
and must be byte-identical across the two trees. No text is stored twice —
the mapping holds hashes only. Old wording of a drifted segment comes from
git history of the markdown page.

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
the same PR once the gate is live.

Machine drafting: `DEEPSEEK_API_KEY=... node tools/docs/translate.ts
translate [page...]` produces isomorphic zh drafts via the DeepSeek API
(no args = only pages missing a zh counterpart; explicit pages overwrite,
feeding the current zh text to the model as terminology reference).
Fenced code inside segments is masked out of the round trip and restored
byte-for-byte. The key comes from the environment and is never stored.
Drafts still go through review and `record`.

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
coverage gaps are already expressed by the generated checklists. The flow is
one-way: an internal item graduates into a doc limitation only once it is
decided to be design (or long-term deferral), and a doc limitation is removed
only when the design changes. Never reference internal IDs, file paths, or
timelines in docs.

The contract went live 2026-09-02: all pages machine-drafted
(deepseek-v4-pro), recorded, `check` green, and the gate wired into the
CI docs check. The legacy hand-written zh tree it replaced survives in
git history.
