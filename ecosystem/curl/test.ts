import { perf, regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-24#5",
            why: "a header naming its includer's types as fields is judged self-contained and opens without its includer",
            kind: "compile",
            detail: /field has incomplete type/,
        },
        {
            issue: "10-25#1",
            why: "the server marks a comment on an `#if 0` line inactive",
            kind: "disagree",
            request: "semanticTokens",
            detail: /\+\s+type = "comment",\n\+\s+modifiers = \["inactive"\]/,
        },
    ],
});

perf("sessions", {}, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index("src/tool_operate.c");
    await p.session(files);
    await p.sample(files);
});
