import { test } from "@clice/tools/testing";

// Every file's preamble ends with a directive; a file warns only where its
// own last line lacks the newline. The preamble's own warnings still show
// past a comment ending its last line.
test(async (t) => {
    const s = await t.start();
    for (const file of ["main.cpp", "config.h", "commented.cpp", "unended.cpp", "bare.h"]) {
        await t.show(file, s.diagnostics(file));
    }
});
