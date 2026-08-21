---
name: triage
description: Classify untriaged issues (open issues without a kind: label) via a GPT batch run, validate against the label taxonomy, and return a proposed-labels report plus an activity digest. Read-only — applying labels happens in the main conversation after maintainer approval. Runs in a forked context.
context: fork
---

Triage one batch of untriaged issues. Untriaged = open issue that carries
`needs-triage` (auto-applied by the issue templates) or has no `kind:`
label (blank issues). The state lives in the labels themselves, so
manually triaged issues are skipped automatically and there is no
bookkeeping file.

## 1. Snapshot

```bash
python3 .claude/skills/triage/scripts/snapshot.py
```

Fetches every untriaged issue (body + comments) into
`/tmp/clice-triage/issues/chunk-N/` (25 per chunk), plus `digest.json`
(new issues in the last 7 days, `needs-info`/`needs-repro` threads with no
activity for over 14 days), `existing-labels.json`, and `titles.json`
(snapshot titles, used by apply to detect drift). Spaces `gh` calls
with `sleep 1` — API rate limits are a real concern. With zero untriaged
issues, skip straight to the digest section of the report.

## 2. Classify

One codex call per chunk; run chunks as parallel background jobs:

```bash
codex_root="$(cd "$(dirname "$(command -v codex)")/.." && pwd)"
systemd-run --user --pipe --wait --collect --same-dir \
  --setenv=PATH="$codex_root/bin:/usr/bin:/bin" \
  -p ProtectHome=tmpfs \
  -p BindReadOnlyPaths="$PWD" \
  -p BindReadOnlyPaths="$codex_root" \
  -p BindPaths="$HOME/.codex" \
  codex exec -m gpt-5.6-sol -c model_reasoning_effort=xhigh \
  --sandbox read-only \
  -o /tmp/clice-triage/verdicts-N.md \
  "Read .claude/skills/triage/rules.md, .github/labels.yml, and every
issue file in /tmp/clice-triage/issues/chunk-N/. Work ONLY from these
local files — no gh, no network. Classify every issue per the rules and
reply with ONLY the JSON array defined by the rules' output schema."
```

Both sandbox layers are mandatory, never the usual full bypass: issue
bodies are untrusted input. Codex's `--sandbox read-only` blocks writes
and command network access but still lets model-run commands read the
whole filesystem, so an injected issue could exfiltrate credentials
through the verdict text. The `systemd-run` wrapper closes that: it
masks `$HOME` and rebinds only the repo (read-only), the codex install
prefix, and `~/.codex` (codex's own state — the one residual exposure).

## 3. Validate

```bash
python3 .claude/skills/triage/scripts/validate.py /tmp/clice-triage/verdicts-*.md
```

Deterministic gate over the model output: every label must exist in
`.github/labels.yml`, exactly one `kind:`, no forbidden additions
(`good first issue`, `help wanted`), no `os:wsl` mixed with a native os
label (existing labels included — such conflicts fail the verdict for
manual resolution), every snapshot issue covered, verdicts for unknown
issues dropped.
Computes `add` = proposed minus existing labels plus a `suggest_remove`
list (existing labels the model omitted — reported for the maintainer,
never auto-removed) and writes `/tmp/clice-triage/validated.json`. A
verdict that fails validation goes to the failures list — report it,
never apply it, and do not hand-edit it back in.

## 4. Report

Return to the main conversation:

- Proposed changes, one line per issue: `#N [conf] +labels — rationale`,
  with `title →` / `ask →` sub-lines where the model proposed them.
- Validation failures and taxonomy gaps.
- Digest: new issues this week, stale waiting threads (with day counts),
  open/untriaged totals.

Do NOT apply anything in the forked run — the maintainer reviews the
proposals first.

## 5. Apply (main conversation, after approval)

```bash
python3 .claude/skills/triage/scripts/apply.py /tmp/clice-triage/validated.json \
  [--only N,N | --skip N,N] [--retitle N,N]
```

Before editing, apply refetches each issue's live labels and title and
reconciles the verdict's full label set against them. Closed issues are
skipped and `needs-triage` is removed. Kind conflicts split on that
marker: while it is still present, template `kind:` labels differing
from the model's are replaced; once the marker is gone, the existing
kind is a maintainer decision — it wins, and both the model's kind and
its derived title rewrite are dropped. An issue whose live labels would
combine `os:wsl` with a native os label is skipped for manual
resolution. Beyond that, labels are only ever added — removals stay
manual via the `suggest_remove` report. Title rewrites apply to
`--retitle all` or explicitly listed issues, and are skipped when the
live title changed after the snapshot. `ask_reporter` suggestions are
never posted automatically — the maintainer sends them personally if
worthwhile.
