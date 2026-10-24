import { perf } from "@clice/tools/ecosystem";

// No regression: comparing inspect's answers with the server's renders each
// of a 250,000-line file's tokens by its line, which takes hours here.
perf("amalgamation", {}, async (p) => {
    await p.index("sqlite3.c");
    await p.sample(["sqlite3.c", "shell.c"]);
    await p.soak("sqlite3.c", { edits: 20 });
});
