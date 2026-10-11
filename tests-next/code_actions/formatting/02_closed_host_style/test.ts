import { test } from "@clice/tools/testing";

// The host source is never opened: its definition is formatted by the path
// the database names, src/main.cpp under the root's LLVM style, not by its
// real path, whose vendor/.clang-format keeps short functions on several
// lines. No compile check: it would read the header's diagnostics alone,
// not the host's.
test({ except: { win32: "creating a symbolic link needs privileges on Windows" } }, async (t) => {
    t.disk.symlink("src", "vendor/real");
    const s = await t.start();
    await s.diagnostics("widget.h");
    await t.show("cls", s.codeActions(t.marker("cls")));
});
