---
name: codex
description: Drive the codex CLI (GPT-5.6-sol) as a delegate — adversarial plan review, code review, debugging, test writing, scoped implementation. Read BEFORE invoking codex.
---

# Codex Delegation

`codex` is an installed CLI agent backed by GPT-5.6-sol — cheap, strong, and
independent of this session's blind spots. Prefer it for: adversarial review of
a design or plan, pre-PR code review, root-causing a bug, adding tests to probe
behavior, and implementing well-scoped tasks. The independence is the value: it
was not part of writing the thing it reviews.

## Invocation

```bash
codex exec -m gpt-5.6-sol -c model_reasoning_effort=xhigh \
  --dangerously-bypass-approvals-and-sandbox \
  -o /tmp/codex-<topic>.md \
  "<prompt>"
```

- Always pass `-o` — it writes the final reply to a file; stdout mixes it into
  the transcript and truncates easily.
- The full bypass is deliberate: the sandbox breaks builds and tooling. Codex
  therefore runs with your permissions — scope the prompt accordingly.
- xhigh runs take minutes to tens of minutes: run in the background and keep
  working, no sleep polling.
- The startup header prints `session id: <uuid>` — capture it whenever a
  follow-up round is plausible.
- If `gpt-5.6-sol` is rejected (plan/auth), drop `-m` to use the account
  default, and say so when reporting results.
- Prompt shape: the task, the exact files/commands in scope, and the answer
  format you want (e.g. "numbered findings, each with a minimal
  counterexample"). Codex reads files itself — point at paths instead of
  pasting content.
- Codex does not auto-load `.claude/` docs — it discovers only `AGENTS.md`,
  which this repo does not have. Any run that should follow project rules
  (review, test writing, implementation) must be told in the prompt which
  rule files to read first, e.g. `.claude/CLAUDE.md` and the cpp-style skill.

The canonical code-review invocation is the standard form with a prompt that
loads the repo rules and reviews the branch diff:

```bash
codex exec -m gpt-5.6-sol -c model_reasoning_effort=xhigh \
  --dangerously-bypass-approvals-and-sandbox -o /tmp/codex-review-<topic>.md \
  "Read .claude/CLAUDE.md and .claude/skills/cpp-style/SKILL.md and apply
their rules. Review the changes in 'git diff origin/main...HEAD' for
correctness, style, and test coverage. Report ranked findings, each with
file:line and a concrete failure scenario."
```

The built-in `codex exec review --base origin/main` collects the diff itself,
but `--base` is mutually exclusive with the prompt argument, so it can never
see the repo rules — use it only as a quick rules-blind supplementary pass
(also `--uncommitted`, `--commit <sha>`).

## Multi-round sessions

`codex exec resume <session-id> "<follow-up>"` continues with full context
(`--last` picks the newest session). Use it for successive adversarial rounds,
"now fix what you found", or clarifying questions — never restate context in a
fresh session. Execution-scoped flags are NOT inherited from the resumed
session: repeat `-m gpt-5.6-sol`, `-c model_reasoning_effort=xhigh`,
`--dangerously-bypass-approvals-and-sandbox`, and a fresh `-o` path on every
resume, or the follow-up silently runs on the default model at default
effort, sandboxed, and without an output file. `codex exec fork <session-id>` branches one history
into independent continuations.

## Discipline

- **Codex output is hypothesis, not verdict.** Every concrete claim ("this
  input breaks it") gets an empirical probe before you act on it; "looks fine"
  carries no weight. Experience runs both ways — codex has correctly refuted
  arguments this side was sure of, and confidently asserted things a probe then
  disproved. The probe decides, never authority.
- **Adversarial loop** (plans/designs): write the doc → codex attacks it
  (demand concrete counterexamples, not general commentary) → probe each
  counterexample → revise the doc, recording adopted and refuted findings →
  `resume` the session for the next round. Stop when a round yields no new
  confirmed finding.
- **When codex edits code** (implementation, debug fixes, new tests): review
  its diff as you would a PR — you own what gets committed. Verification
  (build + suites) happens in the main session, and the hard rules (never
  weaken tests, never push unverified) apply unchanged to codex-authored code.
  Any run that may modify files gets its own git worktree — the main checkout
  is for analysis-only runs, or edits will race with this session's.
- **Never let codex run the integration or snap suites while this session
  might also run them** — concurrent runs in one checkout clobber each other's
  workspace `.clice`. Either codex runs them and you don't, or codex analyzes
  and you verify.

## Recipes

- **Plan review**: point it at the doc path; ask for attacks ranked by
  severity, each with a minimal counterexample. Fold confirmed findings back
  into the doc.
- **Code review**: the canonical review command above — the primary
  self-review pass of the pr skill.
- **Debug**: give the failing test, the repro command, and the suspect area;
  ask for a root-cause hypothesis plus the experiment that would confirm it.
  Let it run the repro itself.
- **Test writing**: point it at the write-tests skill and 2-3 neighboring
  fixtures as the template; ask it to add cases probing a specific behavior
  and report which outputs look wrong versus expected. Suspicious snapshot
  diffs are findings — never `UPDATE_SNAPSHOTS` over them.
- **Implementation**: a well-scoped task with acceptance criteria and pointers
  to the 2-3 existing features whose structure it should copy. Then review and
  verify as above.

## Recovery

If a run dies before writing `-o`, the transcript is at
`~/.codex/sessions/YYYY/MM/DD/*.jsonl`; the final reply is the last record
with payload `type == "message"` and `role == "assistant"`. These transcripts
persist indefinitely and record full prompts, file contents, and command
output — treat `~/.codex/sessions/` as sensitive local data.
