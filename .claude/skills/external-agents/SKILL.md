---
name: external-agents
description: Read before running another vendor's CLI agent — codex (GPT-6 astra), agy (Gemini 4) — as an external reviewer in the pr skill's self-review, a second opinion on a plan, research or a measurement, a debugger, or the implementer of a scoped task. Each only where it is installed and works.
---

# External agents

Agents of other vendors, run as CLIs. Their value is independence: they did not write what they review, and they do not see your conclusions. Each is optional: where one is not installed, not logged in or out of quota, carry on without it and without comment. The one exception is the triage skill, which stops instead: its classification never leaves codex's sandbox.

| Agent   | Model       | Its file   |
| ------- | ----------- | ---------- |
| `codex` | GPT-6 astra | `codex.md` |
| `agy`   | Gemini 4    | `agy.md`   |

Read an agent's file before running it: the invocation, the flags it needs, its pitfalls.

## Prompts

- The task; the files to read in full, and for a change the callers and callees of what changed, not only the diff's lines; the questions; the answer's shape (conclusion first, each finding with file:line, whether it was verified, a minimal counterexample). They read files themselves: point at paths instead of pasting content.
- None of them loads `.claude/`: a run that should follow the project's rules (a review, tests, an implementation) names the rule files to read first — `.claude/CLAUDE.md`, the cpp-style skill, the write-tests skill for test code.
- The material and the questions, never your conclusions: an agent handed a hypothesis tends to confirm it.
- They run commands with your permissions: a read-only run says "do not edit any file".
- A debug run: the failing test, its repro command and the suspect area; ask for a root-cause hypothesis plus the experiment that would confirm it, and let it run the repro itself.

## The review prompt

The pr skill's self-review gives every external agent this prompt, which loads the rules and carries the fixed constraints of the Opus reviewers:

```text
Read .claude/CLAUDE.md, .claude/skills/cpp-style/SKILL.md and, when test code
changed, .claude/skills/write-tests/SKILL.md, and apply their rules. Review the
changes in 'git diff origin/main...HEAD' for correctness, rule violations and
test coverage: read each changed file in full, and the callers and callees of
what changed, not only the diff's lines. Report only defects that lead to a
concrete failure scenario — the input or state, and the wrong result it
produces — each with file:line. Nothing outside this diff's scope. No
refactoring, restructuring or style-preference suggestions. No findings is a
valid answer.
```

## Running

- A run takes minutes to tens of minutes: Bash `run_in_background`, which notifies on exit, and keep working — no sleep polling.
- Keep their raw output out of the main context: read the final answer from its file; progress checks on a long run (log tail, new commits, report landed?) go through an Opus subagent that returns a digest.
- To stop a hung run, kill the PID you recorded at launch (`$!` for a shell background job, the task's PID otherwise) and its subtree — other runs may be live in parallel. When the PID is lost, `pgrep -af '<agent> exe[c]'`-style patterns list the candidates to pick from; never `pkill -f '<agent> ...'`, which matches your own shell's command line and kills it.

## Their answers

- **Hypothesis, not verdict.** Every concrete claim ("this input breaks it") gets an empirical probe before you act on it; "looks fine" carries no weight. They have refuted arguments this side was sure of, and confidently asserted things a probe then disproved: the probe decides, never authority. Where an agent and your own reading disagree, go back to the code. Report which findings held and which did not.
- **A plan or a design**: write the doc → the agent looks for concrete counterexamples, not general commentary → probe each → revise the doc, recording adopted and refuted findings → the next round continues the same session. Stop when a round yields no new confirmed finding.

## When one edits code

- Implementation, debug fixes, new tests: a well-scoped task with acceptance criteria and pointers to the 2-3 existing features whose structure it should copy. Any run that may modify files gets its own git worktree — the main checkout is for read-only runs, or its edits race with this session's.
- Review its diff as you would a PR: you own what gets committed, and the hard rules apply unchanged. Verification (build and suites) happens in the main session.
- An agent that builds or runs suites reads the build-and-test skill first. Never let it run the cases where this session runs them too: two full runs at once slow both. Either it runs them and you don't, or it analyzes and you verify.
