import { perf, regression } from "@clice/tools/ecosystem";

regression();

perf("amalgamation", {}, async (p) => {
    await p.index("sqlite3.c");
    await p.sample(["sqlite3.c", "shell.c"]);
    await p.soak("sqlite3.c", { edits: 20 });
});
