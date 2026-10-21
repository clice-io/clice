import { test } from "@clice/tools/testing";

// Pins the runs that cannot check anything: no database under the
// workspace, a default command no file on disk matches, a workspace that
// does not exist, a worker count that is not a number.
test("no database", { errors: ["Nothing to lint: no compile_commands.json found"] }, async (t) => {
    await t.cli("lint --jobs 2");
});

test(
    "no default source",
    { errors: ["Nothing to lint: no file on disk matches a rule with a default_command"] },
    async (t) => {
        t.disk.write(
            "clice.toml",
            '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++"\n',
        );
        await t.cli("lint --jobs 2");
    },
);

test("missing workspace", async (t) => {
    await t.cli("lint --workspace nowhere --jobs 2");
    await t.show("created", t.disk.list("nowhere/.clice/CACHEDIR.TAG"));
});

test("invalid worker count", async (t) => {
    await t.cli("lint --jobs many");
});
