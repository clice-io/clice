---
name: docs
description: The clice documentation system — generated feature/config pages, how clice runs the shared en↔zh translation checker, and the pixi commands driving them. Read BEFORE editing anything under docs/.
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
- `docs/meta/translations/` — the hash pairs attesting each translated
  segment. Maintained exclusively by `record`; never edit by hand.
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

## Translation contract

The checker is `@clice-io/translate` from the clice-io/docs repository,
shared by every clice-io project with a Chinese tree. Its **RULES.md at
v1.0.0** states the contract, the commands and the editing workflow — read it
before any edit touching translated pages:

    gh api 'repos/clice-io/docs/contents/tools/translations/RULES.md?ref=v1.0.0' -H 'Accept: application/vnd.github.raw'

CI runs `clice-io/docs/check-translations` in the lint workflow's docs
job, pinned to an exact release; the pixi tasks above run that same
release locally (`tools/docs/translations.ts` reads the pin, until the
package is on npm and the tasks call `npx @clice-io/translate@1`). A new
release is adopted by bumping the pin in lint.yml and the RULES.md
references here and in the translate-docs skill. What clice adds on top:

- In clice the frontmatter that translates is index.md's (the VitePress
  home page), and the bold paragraph whose shape must survive is a
  capability card's name; a table row paired with a later heading is a
  capability's status row and its section.
- The formatter of step 4 in RULES.md's workflow is `pixi run format`
  (it canonicalizes table padding, emphasis style and CJK spacing).
- The workflow applies to generated regions too: after
  `update-feature-docs` changes an en feature page, the zh page must
  receive the translated equivalent in the same PR — batched at the end of
  the branch, see below.
- The wording — clice's page conventions and glossary — is the
  translate-docs skill. `review-doc-translations [page...]` passes that
  skill to the model as the glossary; review its diff, then `format` and
  `record`. Prefer it over handing a model whole pages: the code blocks
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
   pages, or dozens of drifted pages, copy the en page over the zh one
   and run `review` on it (machine drafting, RULES.md).
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
untranslated pages, the checker takes `--ignore=GLOB` (none today): a
matching page needs no zh counterpart and no mapping; pass it to the pixi
tasks and the action's `ignore` input alike.

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

The contract went live 2026-09-02: all pages machine-drafted, recorded,
`check` green, and the gate wired into the CI docs check. The legacy hand-written zh tree it replaced survives in
git history.
