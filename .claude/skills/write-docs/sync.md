# Syncing the docs at the end of a branch

Generated regions and translations are synced **once per branch, right
before the checks** of the pull-request skill — not after every case or
page edit, and not in the main conversation: delegate it to an Opus
subagent so the report output and page texts never enter the main context. Give the
subagent the write-docs skill and `git diff --name-only origin/main...HEAD`; its
brief is:

1. If documented cases or config annotations changed:
   `pixi run docs-update` rewrites the en GENERATED regions. The
   configuration page renders the committed schema: after a config
   annotation changed, first rewrite it with the clice built from the
   branch, `build/RelWithDebInfo/bin/bin/clice inspect config-schema >
docs/public/clice-config.schema.json` (on a box, run there and
   `box cp $id . docs/public/clice-config.schema.json` back).
2. `pixi run docs-report` lists every broken pair with both
   texts. A zh segment edited alone, or with its en one, is checked
   against the en text for meaning before it is recorded. Translate
   each new or drifted en segment into the zh page,
   keeping the skeleton (same block kind, list marker, heading depth,
   nested code byte-identical) and the terminology of the surrounding
   page; delete zh segments whose en segment is gone. For whole new
   pages, or dozens of drifted pages, copy the en page over the zh one
   and run `review` on it (machine drafting, RULES.md).
3. `pixi run format`, then `pixi run docs-record`, then
   `pixi run docs-check` — green.
4. Report back: pages touched, how many segments were translated, and
   anything deliberately left as is.
