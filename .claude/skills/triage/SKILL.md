---
name: triage
description: Use when asked to triage issues — classifies untriaged issues with codex, validates the labels, and returns proposed labels plus an activity digest. Read-only; runs in a forked context.
context: fork
---

Triage one batch of untriaged issues: issues without the `triaged` marker,
which apply stamps on every processed issue (title patterns and kind labels
prove nothing — reporters set both). The issue templates add `needs-triage`
as a visible awaiting signal; apply removes it. The state lives in the
labels, so there is no bookkeeping file.

The classification needs codex logged in with quota (`codex login status`).
Without it, stop and say so: issue bodies are untrusted input, so the
classification never moves to an unsandboxed agent.

## 1. Snapshot

```bash
python3 .claude/skills/triage/scripts/snapshot.py
```

Fetches every open untriaged issue (body + comments) into
`/tmp/clice-triage/issues/chunk-N/` (25 per chunk), plus `digest.json`
(new issues in the last 7 days, `needs-info`/`needs-repro` threads with no
activity for over 14 days), `existing-labels.json`, `titles.json` and
`states.json` (what apply checks for drift). `--state all` sweeps closed
issues too, paired with apply's `--include-closed`, for a backfill. With
zero untriaged issues, skip straight to the digest section of the report.

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
  codex exec -m gpt-6-astra -c model_reasoning_effort=xhigh \
  --sandbox read-only \
  -o /tmp/clice-triage/verdicts-N.md \
  "Read .claude/skills/triage/rules.md, .github/labels.yml, and every
issue file in /tmp/clice-triage/issues/chunk-N/. Work ONLY from these
local files — no gh, no network. Classify every issue per the rules and
reply with ONLY the JSON array defined by the rules' output schema." < /dev/null
```

Both sandbox layers are mandatory, never the usual full bypass. Codex's
`--sandbox read-only` blocks writes and command network access but still
lets model-run commands read the whole filesystem, so an injected issue
could exfiltrate credentials through the verdict text. The `systemd-run`
wrapper masks `$HOME` and rebinds only the repo (read-only), the codex
install prefix, and `~/.codex` (codex's own state — the one residual
exposure).

## 3. Validate

```bash
python3 .claude/skills/triage/scripts/validate.py /tmp/clice-triage/verdicts-*.md
```

A deterministic gate over the model output: every label exists in
`.github/labels.yml`, exactly one `kind:`, no `good first issue` or
`help wanted`, no `os:wsl` beside a native os label, every snapshot issue
covered. It writes `/tmp/clice-triage/validated.json` with `add` (proposed
minus existing labels) and `suggest_remove` (existing labels the model
omitted — reported, never removed). A verdict that fails validation is
reported, never applied, and never hand-edited back in.

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
  [--only N,N | --skip N,N] [--retitle N,N | --retitle all] [--include-closed]
```

apply refetches each issue's live labels and title. It skips an issue
whose state changed since the snapshot, that gained `triaged` meanwhile,
a closed one without `--include-closed`, and one that needs manual
resolution (several maintainer kinds, `os:wsl` beside a native os label).
While `needs-triage` is present the model's kind replaces the template's;
once it is gone the existing kind is the maintainer's and wins, and the
model's kind, `status:` labels and title are dropped. A closed issue loses
every `status:` label. Beyond that apply only adds labels — removals stay
manual via `suggest_remove` — and stamps `triaged` in a separate final
call, so a failed edit leaves the issue visible to the next snapshot.
Titles change only for the issues `--retitle` names whose title did not
change since the snapshot. `ask_reporter` suggestions are never posted —
the maintainer sends them personally if worthwhile.
