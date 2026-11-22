# agy

`agy` runs Google's Gemini 4 models.

```bash
agy --model gemini-4-neon-high --dangerously-skip-permissions \
  --output-format stream-json --print="<prompt>" > /tmp/agy-<topic>.jsonl 2> /tmp/agy-<topic>.err
jq -r 'select(.event == "result") | .result | if .status == "SUCCESS" then .response else error("agy ended \(.status): \(.error)") end' \
  /tmp/agy-<topic>.jsonl > /tmp/agy-<topic>.md
```

- A run that ended otherwise (`ERROR`, with its `error`) or has no result line failed: it is no review, and an empty report is no "no findings". The `jq` above exits non-zero on the first, and writes nothing on the second.

- The prompt goes in `--print=` itself: as a separate argument, `--model` takes it. A long prompt lives in a file: `--print="$(cat /tmp/agy-<topic>-prompt.txt)"`.
- Print mode answers only when the whole turn ends, typically after 10–40 minutes; meanwhile the file's `step_update` lines show what it is doing.
- `gemini-4-neon-high` is the default; `gemini-4-carbon-high` and `gemini-4-argon-high` are other models, for another view of a design question.
