import { perf, regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-25#1",
            why: "the server marks a comment on an `#if 0` line inactive",
            kind: "disagree",
            request: "semanticTokens",
            // The comment's token, and the same with the modifier.
            detail: /^-\s+\{ at = "#if 0 [^\n]*", type = "comment" \},\n\+\s+(?:\{ at = "#if 0 [^\n]*", type = "comment", modifiers = \["inactive"\] \},|\{\n\+\s+at = "#if 0 [^\n]*",\n\+\s+type = "comment",\n\+\s+modifiers = \["inactive"\],\n\+\s+\},)$/m,
        },
    ],
});

perf("sessions", {}, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index("src/tool_operate.c");
    await p.session(files);
    await p.sample(files);
});
