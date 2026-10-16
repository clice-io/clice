import { test } from "@clice/tools/testing";

// Pins HeaderFilterRegex against a header reached through a linked include
// directory: the filter matches the path the include found, inc/, not the
// file the link points to.
test(
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("inc", "vendor/real");
        await t.cli("lint --jobs 2");
    },
);
