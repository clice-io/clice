---
name: write-docs
description: Read before writing or editing anything under docs/ — the generated pages, the en↔zh contract and the pixi commands that check it; sync.md for the end-of-branch docs sync, glossary.md for the Chinese pages' wording.
---

# Writing the docs

Beside this file: `sync.md`, the docs sync at the end of a branch; `glossary.md`, the wording of the Chinese pages.

## Layout and sources of truth

- `docs/en/` — English pages, written by hand except their GENERATED
  regions: the cards of the documented cases (`tests/README.md`, "Docs") on
  the feature, CLI and guide pages, and the option lists of
  guide/configuration.md, rendered from the config schema.
  **Never edit inside a `<!-- BEGIN GENERATED ... -->` region** — edit the
  case's `README.md` or the config annotations instead.
- `docs/cards/` — one card per documented case, written by every cases run
  and ignored by git; publishing (docs.yml) and the site build of pull
  requests (main.yml's `docs-site`) take the cards of CI's linux-x64 run
  (test.yml's `docs-cards`).
- `docs/zh/` — Chinese pages, equally real and equally hand-edited (by a
  person or a model). Each zh page must stay **segment-isomorphic** to its
  en counterpart: same sequence of markdown blocks, translated text in the
  translatable blocks, code blocks and HTML comments byte-identical.
- `docs/meta/translations/` — the hash pairs attesting each translated
  segment. Maintained exclusively by `record`; never edit by hand.
- Each tree has its own hand-maintained `sidebar.yaml`.
- `docs/public/clice-config.schema.json` — committed output of
  `clice inspect config-schema`; CI checks freshness.

## Commands (pixi)

| command                | what it does                                                                                                         |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `pixi run docs-update` | rewrite every GENERATED region: the feature pages' from the cases, the configuration page's from the schema          |
| `pixi run docs-check`  | what CI's lint docs job checks: the GENERATED regions are current, zh is isomorphic to en and all pairs are attested |
| `pixi run docs-report` | translator worklist: drifted segments with texts                                                                     |
| `pixi run docs-record` | re-attest hash pairs after deliberate edits                                                                          |
| `pixi run docs-review` | model review of zh pages, segment by segment                                                                         |

## Translation contract

The checker is `@clice-io/translate` from the clice-io/docs repository,
shared by every clice-io project with a Chinese tree. Its **RULES.md at
v1.0.0** states the contract, the commands and the editing workflow — read it
before any edit touching translated pages:

    gh api 'repos/clice-io/docs/contents/tools/translations/RULES.md?ref=v1.0.0' -H 'Accept: application/vnd.github.raw'

The pixi tasks above run it at an exact release, locally and in the lint
workflow's docs job alike: `tools/docs/translations.ts` holds the pin,
until the package is on npm and the tasks call `npx @clice-io/translate@1`.
A new release is adopted by bumping that pin and the RULES.md reference
here. What clice adds on top:

- In clice the frontmatter that translates is index.md's (the VitePress
  home page), and the bold paragraph whose shape must survive is a
  capability card's name; a table row paired with a later heading is a
  capability's status row and its section.
- The formatter of step 4 in RULES.md's workflow is `pixi run format`
  (table padding, emphasis style). The spacing between Chinese and Latin
  text is the writer's: CI's lint docs job checks it (autocorrect).
- The workflow applies to generated regions too: after
  `docs-update` changes an en page, the zh page must
  receive the translated equivalent in the same PR — batched at the end of
  the branch (`sync.md`).
- `pixi run docs-review [page...]` has a model (codex, the external-agents
  skill) review the zh pages segment by segment against `glossary.md`;
  review its diff, then `format` and `record`. Prefer it over handing a
  model whole pages: the code blocks would only burn its context.

**docs/ contains no changelog content** — neither per-page "Changelog"
sections nor changelog pages. Feature history lives in git and PRs; LLVM
upgrade notes live in the upgrade-llvm skill's `llvm-changelog.md`.

## What belongs in a "Known Limitations" section

Design-level, user-visible trade-offs that are stable on a months timescale,
written in behavior terms — they answer the reader's "why does it work this
way". Bugs never go there: they live in the issues and disappear when fixed;
in docs they would go stale. Feature coverage gaps are already expressed by
the generated status tables. An issue becomes a doc limitation only once it is
decided to be design (or long-term deferral), and a doc limitation is removed
only when the design changes. Never reference internal IDs, file paths, or
timelines in docs.
