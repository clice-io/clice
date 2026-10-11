import { test } from "@clice/tools/testing";

// Pins where a linked source finds its .clang-tidy: above the name the
// database gives it, src/, whose checks are all off; not above the file the
// link points to, under vendor/, whose checks would report the division.
test(
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("src", "vendor/real");
        t.disk.database({ "src/main.cpp": [] });
        await t.cli("lint --workers 2");
    },
);
