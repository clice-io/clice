/// The server's test hooks: sync waits out every request, build and index
/// round, stats counts the builds per file, and a hold parks a build's reply
/// while the server still sees it in flight.

import * as proto from "vscode-languageserver-protocol";
import { HoldRequest, type BuildKind } from "@clice/tools/protocol";
import { expect, test } from "../fixtures.ts";

const UNITS = ["a.cpp", "b.cpp", "c.cpp"];

test("sync waits for indexing", async ({ session }) => {
    const { client, workspace } = session.tmp();
    for (const unit of UNITS) {
        workspace.write(unit, `int fn_${unit[0]}() { return 1; }\n`);
    }
    workspace.writeCDB(UNITS);
    await client.initialize(workspace);

    expect(await client.sync()).toEqual({ failed: [], unsaved: false, pending: [] });
    const symbols = (await client.workspaceSymbols("fn_")) ?? [];
    expect(symbols.map((symbol) => symbol.name).sort()).toEqual(["fn_a", "fn_b", "fn_c"]);
    const builds = (await client.stats()).builds;
    expect(builds.map((build) => [client.normalizeUri(build.uri), build.index]).sort()).toEqual(
        UNITS.map((unit) => [workspace.uri(unit), 1]),
    );
});

test("sync after a poll", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("a.cpp", "int fn_a() { return 1; }\n");
    workspace.writeCDB(["a.cpp"]);
    await client.initialize(workspace);
    await client.sync();

    // A rewrite of another size: the look at the disk sees it at once.
    workspace.write("a.cpp", "int fn_renamed() { return 1; }\n");
    expect(await client.sync({ poll: true })).toEqual({ failed: [], unsaved: false, pending: [] });
    expect((await client.workspaceSymbols("fn_renamed"))?.length).toBe(1);
});

test("sync waits for requests", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return missing; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    // The pull waits for a compile no build had started when the syncs
    // arrived; neither sync waits for the other.
    const [uri] = client.open("main.cpp");
    let answered = false;
    const pulled = client.pullDiagnostics(uri).then(() => {
        answered = true;
    });
    const syncs = await Promise.all([client.sync(), client.sync()]);
    expect(answered, "the pull is answered before the syncs").toBe(true);
    expect(syncs.map((sync) => sync.pending)).toEqual([[], []]);
    await pulled;
});

test("sync reports held work", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("a.cpp", "int fn_a() { return 1; }\n");
    workspace.writeCDB(["a.cpp"]);
    await client.initialize(workspace);
    await client.sync();

    // The hold is in place before the database names b.cpp.
    workspace.write("b.cpp", "int fn_b() { return 2; }\n");
    workspace.writeCDB(["a.cpp", "b.cpp"]);
    const hold = await client.hold("index", workspace.uri("b.cpp"));
    await client.poll("cdb");
    await client.parkedBy(hold);

    const held = await client.sync({ deadlineMs: 1_000 });
    const file = workspace.displayPath("b.cpp");
    expect(held.pending.slice(0, 2)).toEqual([
        `index ${file}: reply parked by hold ${hold}`,
        `index ${file}`,
    ]);
    expect(held.pending[2]).toMatch(/^index round at \d+\/\d+, 0 files queued$/);

    await client.release(hold);
    expect((await client.sync()).pending).toEqual([]);
    expect((await client.workspaceSymbols("fn_b"))?.length).toBe(1);
});

test("hold parks a compile", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return missing; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    await client.sync();

    const [uri] = client.open("main.cpp");
    const hold = await client.hold("compile", uri);
    // A pull waits for the compile; a hover could take the index's rows.
    const pulled = client.pullDiagnostics(uri);
    await client.parkedBy(hold);
    expect(client.publishCount(uri), "a parked compile publishes nothing").toBe(0);
    const file = workspace.displayPath("main.cpp");
    expect((await client.sync({ deadlineMs: 1_000 })).pending).toEqual([
        expect.stringMatching(/^request textDocument\/diagnostic \d+$/),
        `compile ${file}: reply parked by hold ${hold}`,
        `compile ${file}`,
    ]);

    const published = client.armDiagnostics(uri);
    await client.release(hold);
    await published;
    await pulled;
    expect(client.errors(uri)).toHaveLength(1);
    const builds = (await client.stats()).builds.find(
        (build) => client.normalizeUri(build.uri) === uri,
    );
    expect(builds?.compile).toBe(1);
});

test("shutdown releases a parked reply", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    await client.sync();

    const [uri] = client.open("main.cpp");
    const hold = await client.hold("compile", uri);
    const hover = client.hoverAt(uri, 0, 4).catch(() => null);
    await client.parkedBy(hold);
    // The exit gate fails a server that does not exit.
    await client.shutdown();
    await hover;
});

test("holds name a build and a file", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const invalid = { code: proto.ErrorCodes.InvalidParams };
    await expect(
        client.sendRequest(HoldRequest, {
            kind: "query" as BuildKind,
            uri: workspace.uri("main.cpp"),
        }),
    ).rejects.toMatchObject(invalid);
    await expect(client.hold("compile", "untitled:main")).rejects.toMatchObject(invalid);
    await expect(client.release(7)).rejects.toMatchObject(invalid);
});

test("hooks need the option", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, {
        initializationOptions: { project: { test_hooks: false } },
    });
    const off = { code: proto.ErrorCodes.InvalidRequest };
    await expect(client.sync()).rejects.toMatchObject(off);
    await expect(client.hold("compile", workspace.uri("main.cpp"))).rejects.toMatchObject(off);
    await expect(client.release(1)).rejects.toMatchObject(off);
    await client.openAndWait("main.cpp");
    expect((await client.stats()).builds, "no build is counted").toEqual([]);
});
