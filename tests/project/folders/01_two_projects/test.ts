import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

const FOLDERS = { folders: ["alpha", "beta"] };
const PROGRESS = "clice/backgroundIndex";

/// Whether the directory holds any file.
function exists(t: TestContext, dir: string): boolean {
    return t.disk.list(`${dir}/**`).length > 0;
}

test("folders compile separately", async (t) => {
    const s = await t.start(FOLDERS);
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
    // The gauges add every folder up.
    expect((await s.stats()).sessions).toBe(2);
    expect(await s.poll("cdb")).toBe(0);
});

test("root uri alone serves its folder", async (t) => {
    // A client without folder support: the root's project finds the
    // database below it.
    const s = await t.start({ folders: null });
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
});

test("workspace symbol spans folders", async (t) => {
    const s = await t.start(FOLDERS);
    s.open("alpha/main.cpp");
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("_fn"));
});

test("each folder keeps its own cache", async (t) => {
    const s = await t.start(FOLDERS);
    await s.indexed();
    await s.stop();
    // The client's cache directory went to the first folder; the second
    // kept its default.
    expect(exists(t, ".clice")).toBe(true);
    expect(exists(t, "alpha/.clice")).toBe(false);
    expect(exists(t, "beta/.clice")).toBe(true);
});

test("unclaimed file opens its project", async (t) => {
    // Outside every folder, with a database above it: that folder is
    // served as if it were open.
    const s = await t.start({ folders: ["alpha"] });
    expect(await s.errors("beta/main.cpp")).toEqual([]);
});

test("rootless server finds projects", async (t) => {
    const s = await t.start({ folders: [] });
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
});

test("folder change before initialized", async (t) => {
    const s = await t.start({
        folders: ["alpha"],
        foldersBeforeInitialized: { added: ["beta"], removed: ["alpha"] },
    });
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("_fn"));
});

test("added folder adopts its files", async (t) => {
    t.disk.database({ "alpha/main.cpp": ["-DIN_ALPHA"] }, "alpha");
    const s = await t.start({ folders: ["alpha"] });
    // Nothing above it knows the file: the first project serves it,
    // without beta's flags.
    await t.show("alpha serves", s.errors("beta/main.cpp"));
    t.disk.database({ "beta/main.cpp": ["-DIN_BETA"] }, "beta");
    await s.changeFolders({ added: ["beta"] });
    await s.sync();
    expect(await s.errors("beta/main.cpp")).toEqual([]);
});

test("a restart keeps the folders changed since", async (t) => {
    const s = await t.start({ folders: ["alpha"] });
    await s.changeFolders({ added: ["beta"] });
    await s.restart();
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("_fn"));
});

test("removed folder releases its files", async (t) => {
    const s = await t.start(FOLDERS);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
    // A moved document compiles again on its own: the client sends nothing
    // that would replace what the old project pushed.
    await s.changeFolders({ removed: ["beta"] });
    await s.sync();
    await s.changeFolders({ added: ["beta"] });
    await s.sync();
    await t.show("pushes", s.pushes("beta/main.cpp"));
});

test("folder re-added at once keeps its cache", async (t) => {
    const s = await t.start(FOLDERS);
    await s.indexed();
    await s.changeFolders({ removed: ["beta"] });
    await s.changeFolders({ added: ["beta"] });
    await s.sync();
    // Only a project holding the cache directory's writer lock answers
    // `clice index`.
    await t.cli("index --workspace beta --jobs 1");
});

test("database change moves a document", async (t) => {
    t.disk.write("alpha/shared.cpp", "#ifndef IN_BETA\n#error missing IN_BETA\n#endif\n");
    const s = await t.start(FOLDERS);
    await t.show("unlisted", s.errors("alpha/shared.cpp"));
    // Beta's database starts listing the file: it moves there.
    t.disk.database({ "beta/main.cpp": ["-DIN_BETA"], "alpha/shared.cpp": ["-DIN_BETA"] }, "beta");
    expect(await s.poll("cdb")).toBe(1);
    await s.sync();
    expect(await s.errors("alpha/shared.cpp")).toEqual([]);
});

test("removed first folder hands over", async (t) => {
    const s = await t.start(FOLDERS);
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
    await s.changeFolders({ removed: ["alpha"] });
    await s.sync();
    // Beta serves the file, without alpha's flags, and keeps serving its own.
    await t.show("alpha", s.errors("alpha/main.cpp"));
    await t.show("beta", s.hover(t.at("beta/main.cpp", "int |beta_fn")));
});

test("removed only folder goes rootless", async (t) => {
    const s = await t.start({ folders: ["alpha"] });
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    await s.changeFolders({ removed: ["alpha"] });
    await s.sync();
    // The rootless project guesses a command.
    await t.show("guessed", s.errors("alpha/main.cpp"));
});

test("batch index asks the folder's server", async (t) => {
    const s = await t.start(FOLDERS);
    await s.indexed();
    await t.cli("index --workspace beta --jobs 1");
});

test("indexing progress ends across folders", async (t) => {
    t.disk.write("empty/notes.txt", "");
    const s = await t.start({
        folders: ["alpha", "empty", "beta"],
        capabilities: { window: { workDoneProgress: true } },
    });
    // A round is shown only once the client took its token: beta's unit
    // held, the round outlasts that handshake. The progress the client
    // was sent is read raw: it is no request of the test's.
    const held = await s.park("index", "beta/main.cpp", { at: "reply" });
    s.open("alpha/main.cpp");
    await s.client.progressed(PROGRESS, "begin");
    await held.release();
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("_fn"));
    // A folder with nothing to index never runs a round; the others'
    // rounds still end the one the client sees: every round shown began
    // and ended, the last one too.
    const kinds = s.client.progressEvents
        .filter((event) => event.token === PROGRESS)
        .map((event) => (event.value as { kind: string }).kind)
        .filter((kind) => kind !== "report");
    expect(kinds.at(-1)).toBe("end");
    expect(kinds).toEqual(kinds.map((_, index) => (index % 2 === 0 ? "begin" : "end")));
});

test("restart serves every folder", async (t) => {
    const s = await t.start(FOLDERS);
    await s.indexed();
    await s.restart();
    // Both folders load their persisted index at startup: the first query
    // answers before any worker could index either again.
    await t.show("symbols", s.workspaceSymbols("_fn"));
});
