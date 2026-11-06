import { test } from "@clice/tools/testing";

// The database names the file through a link to its directory, as Bazel's
// execroot does, and the editor opens it where it is: a completion or a
// signature help asked where nothing is typed after `.` or `(` still reaches
// its point.
test(async (t) => {
    t.disk.linkDirectory("exec/src", "src");
    const s = await t.start();
    s.open("src/main.cpp");
    await t.show("member", s.completion(t.marker("member"), "."));
    await t.show("typed", s.completion(t.marker("typed")));
    await t.show("arguments", s.signatureHelp(t.marker("arguments")));
});
