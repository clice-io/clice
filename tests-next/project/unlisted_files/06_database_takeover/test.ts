import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const NEW =
    "#ifndef LISTED\nint regen_fn() { return 1; }\n#else\nint listed_fn() { return 2; }\n#endif\n";

test("database takes over, then drops", async (t) => {
    const s = await t.start({ config: { project: { idle_timeout_ms: 10 } } });
    s.open("src/a.cpp");
    t.disk.write("src/new.cpp", NEW);
    await s.diagnostics("src/new.cpp");
    s.save("src/new.cpp");
    s.close("src/new.cpp");
    await s.indexed();
    await t.show("saved", s.workspaceSymbols("_fn"));

    // The entry's command takes over.
    t.disk.database({ "src/a.cpp": [], "src/s.cpp": [], "src/new.cpp": ["-DLISTED"] });
    expect(await s.poll("cdb")).toBe(1);
    await s.indexed();
    await t.show("listed", s.workspaceSymbols("_fn"));

    // The file leaves the index with its entry.
    t.disk.database({ "src/a.cpp": [], "src/s.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    await s.indexed();
    await t.show("dropped", s.workspaceSymbols("_fn"));
    // Queued after the reload: a record outliving the takeover would have
    // the file indexed again by the time this lands.
    t.disk.write("src/s.cpp", "int s() { return 0; }\nint sentinel_fn() { return 3; }\n");
    await s.sync({ poll: true });
    await s.indexed();
    await t.show("no record", s.workspaceSymbols("_fn"));

    // A save records it again.
    s.open("src/new.cpp");
    s.save("src/new.cpp");
    s.close("src/new.cpp");
    await s.indexed();
    await t.show("saved again", s.workspaceSymbols("_fn"));
});
