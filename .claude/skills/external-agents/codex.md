# codex

`codex` runs OpenAI's GPT-6 astra. It is usable when `codex login status` succeeds and a run does not stop on a usage limit.

```bash
codex exec -m gpt-6-astra -c model_reasoning_effort=xhigh \
  --dangerously-bypass-approvals-and-sandbox \
  -o /tmp/codex-<topic>.md \
  "<prompt>" < /dev/null
```

- Always pass `-o` — it writes the final reply to a file; stdout mixes it into the transcript and truncates easily. `-o` holds only the last assistant message: never point it at a file the prompt asks codex to write its report to (or one it might guess) — the message overwrites the report. When the prompt names a report file, give `-o` a clearly different name (`*-final-message.md`).
- The full bypass is deliberate: the sandbox breaks builds and tooling.
- A background `codex exec` must get `< /dev/null`: with a non-TTY stdin it reads the pipe for extra input and blocks forever (the prompt argument does not help). The same applies to Node — spawn it with `stdio: ["ignore", ...]`. Right after launch, `head` the log and confirm the `session id: <uuid>` header appeared — a live process alone proves nothing — and keep the id whenever a follow-up round is plausible.
- If `gpt-6-astra` is rejected (plan/auth), drop `-m` to use the account default, and say so when reporting results.
- `-c service_tier=fast` switches the run to the fast service tier; use it when the maintainer asks for speed (long migrations, translation passes).
- Codex discovers only `AGENTS.md`, which this repo does not have.
- The review: the review prompt of the skill as `"<prompt>"`, output to `-o /tmp/codex-review-<topic>.md`. `codex exec review --base origin/main` cannot take a prompt (`--base` and the prompt argument are mutually exclusive), so it never sees the repo rules — not for self-review.

## Rounds

`codex exec resume <session-id> "<follow-up>"` continues with full context (`--last` picks the newest session): successive review rounds, "now fix what you found", clarifying questions — never restate context in a fresh session, or the round re-reports what is already known. Execution-scoped flags are NOT inherited from the resumed session: repeat `-m gpt-6-astra`, `-c model_reasoning_effort=xhigh`, `--dangerously-bypass-approvals-and-sandbox`, and a fresh `-o` path on every resume, or the follow-up silently runs on the default model at default effort, sandboxed, and without an output file. `codex exec fork <session-id>` branches one history into independent continuations.

## Wording

Prompts and everything codex reads go through OpenAI's cybersecurity filter, which judges phrasing, not intent, and kills the session mid-run (the content stays in that session's history, so `resume` trips it again — only a fresh session recovers).

- Use QA vocabulary: "review", "latent correctness defects", "regression check", "counterexample". Never "attack", "attack surface", "hunt/dig for bugs", "fabricate the conditions for the bug"; a negative control is "a control variant expected to fail". Open with the fact that this is routine quality work on our own open-source project (the repo's git config and LICENSE back it up).
- Do not feed codex internal notes written in hunting slang — extract the scope into a neutral file and keep the notes' edits on this side. Ask for terse, report-only output: long analytical prose triggers the filter too.

## A run that died

If a run dies before writing `-o`, the transcript is at `~/.codex/sessions/YYYY/MM/DD/*.jsonl`; the final reply is the last record with payload `type == "message"` and `role == "assistant"`. These transcripts persist indefinitely and record full prompts, file contents, and command output — treat `~/.codex/sessions/` as sensitive local data.
