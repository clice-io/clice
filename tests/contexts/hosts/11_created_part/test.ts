import { test } from "@clice/tools/testing";

// A file created and opened at once compiles in its host's context from its
// first diagnostics, the pull read in the same turn as the open: the open
// applies the disk change that gives the host its include first.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("unity.cpp");
    t.disk.write(
        "part.cpp",
        "#ifndef IN_UNITY\n#error compiled alone\n#endif\n\nint part() { return 0; }\n",
    );
    await t.show("diagnostics", s.diagnostics("part.cpp"));
    await t.show("context", s.compileCommand("part.cpp"));
});
