import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// A file reached through a link is looked up where the link leads: the
// database beside its target lists it, though discovery from the
// workspace root does not reach that deep.
test(
    "through a file link",
    { except: { win32: "creating symlinks needs privileges" }, errors: [MISSING] },
    async (t) => {
        t.disk.symlink("lib.cpp", "deep/src/lib.cpp");
        await t.cli("command lib.cpp");
    },
);
