import { perf, regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-24#7",
            why: "in templates and macro bodies the open file's highlights hold more than the unit's own",
            kind: "disagree",
            request: "highlights",
            detail: /^(?![\s\S]*\n-\s+\{)/,
        },
    ],
});

perf("sessions", {}, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index(files[0] ?? "");
    await p.session(files);
    await p.sample(files);
});
