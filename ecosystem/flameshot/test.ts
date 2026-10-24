import { perf, regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-24#10",
            why: "a completion label spells an overriding method's default argument as the overridden Qt one does on the server alone",
            kind: "disagree",
            request: "completion",
            detail: /= Q_NULLPTR[^\n]*\n\+[^\n]*= nullptr/,
        },
    ],
});

perf("sessions", {}, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index("src/main.cpp");
    await p.session(files);
    await p.sample(files);
});
