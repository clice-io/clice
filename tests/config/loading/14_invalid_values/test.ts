import { test } from "@clice/tools/testing";

// Values of the right type that clice cannot honor are errors at their
// place: a readonly mode that is not one of "off", "on" and "auto" (case
// matters), a pattern that is no glob or holds a backslash, a worker count
// or a limit of 0.

test("readonly", async (t) => {
    t.disk.write("clice.toml", '[[rules]]\nreadonly = "On"\n');
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

test("glob", async (t) => {
    t.disk.write("clice.toml", '[[rules]]\npatterns = ["src/[a"]\nappend = ["-DX"]\n');
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

test("backslash", async (t) => {
    t.disk.write("clice.toml", '[[rules]]\npatterns = ["src\\\\**"]\nappend = ["-DX"]\n');
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

// The test client sets foreground and background itself, and the editor's
// settings win over the file's.
test("worker count", async (t) => {
    t.disk.write("clice.toml", "[workers]\nmax = 0\n");
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

test("limit", async (t) => {
    t.disk.write("clice.toml", "[references]\nlimit = 0\n");
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});
