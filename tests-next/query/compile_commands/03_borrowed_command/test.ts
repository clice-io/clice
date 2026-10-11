import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// A header no source includes takes its command from a source whose
// include path finds it: a/inc reaches vendor/real through a symlink, and
// the header, though it sits beside vendor/b.cpp, borrows a/main.cpp's.
test(
    "borrowed through a symlink",
    { except: { win32: "creating symlinks needs privileges" }, errors: [MISSING] },
    async (t) => {
        t.disk.symlink("a/inc", "vendor/real");
        await t.cli("index");
        await t.cli("query --method compileCommand --path a/inc/orphan.h");
    },
);
