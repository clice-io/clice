import { test } from "@clice/tools/testing";

// Values of the right type that clice cannot honor are errors at their
// place: a readonly mode that is not one of "off", "on" and "auto" (case
// matters), a pattern that is no glob, a worker count of 0.
const INVALID = { errors: ["Invalid clice\\.toml"] };

test("readonly", INVALID, async (t) => {
    t.disk.write("clice.toml", '[project]\nreadonly = "On"\n');
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

test("glob", INVALID, async (t) => {
    t.disk.write("clice.toml", '[[rules]]\npatterns = ["src/[a"]\nappend = ["-DX"]\n');
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

test("worker count", INVALID, async (t) => {
    t.disk.write("clice.toml", "[project]\nmin_stateless_worker_count = 0\n");
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});
