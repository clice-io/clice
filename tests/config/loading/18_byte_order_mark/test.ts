import { test } from "@clice/tools/testing";

// A clice.toml saved as UTF-8 with a byte order mark loads, and a warning on
// its first line is where the editor shows the text.
test(async (t) => {
    t.disk.write("clice.toml", `\ufeff${t.disk.read("clice.toml")}`);
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});
