import { test } from "@clice/tools/testing";

// The database lists a fetched dependency's source beside the project's own.
test("build tree", async (t) => {
    await t.cli("format");
});

test(
    "build tree through a root link",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("compile_commands.json", "build/compile_commands.json");
        await t.cli("format");
    },
);
