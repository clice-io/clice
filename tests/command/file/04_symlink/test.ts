import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// A file asked for through a symlinked directory is the file the database
// lists: the rule matching its real path applies.
test(
    "through a symlink",
    { except: { win32: "creating symlinks needs privileges" }, errors: [MISSING] },
    async (t) => {
        t.disk.symlink("link", "real");
        await t.cli("command link/main.cpp");
    },
);
