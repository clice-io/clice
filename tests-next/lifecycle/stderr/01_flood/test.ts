import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// Editors drain the server's stderr; one that does not costs lines of the
// stderr mirror, never the server's liveness and never a line of its log
// file. The volume comes from a test hook, clice/internal/logFlood, so it
// does not hang on what features happen to log.

const BATCHES = 10;
const LINES = 300;

function floodLines(text: string): number {
    return text.split("[stderr-flood ").length - 1;
}

test("the flood needs the hooks", async (t) => {
    const s = await t.start({ config: { project: { test_hooks: false } } });
    const refused = await s.client.logFlood(1, 16).then(
        () => null,
        (error: unknown) => error,
    );
    expect(refused).toBeInstanceOf(proto.ResponseError);
    const { code, message } = refused as proto.ResponseError<unknown>;
    await t.show("refused", { code, message });
});

test("a flood never wedges", async (t) => {
    const s = await t.start({ stderr: "unread" });
    const client = s.client;
    const value = t.at("main.cpp", "int |value");
    s.open("main.cpp");
    // About 1 MB in ten batches, far past the pipe and the server's own
    // buffer; a hover after each batch is answered all the same.
    const first = await s.hover(value);
    for (let batch = 0; batch < BATCHES; batch++) {
        await client.logFlood(LINES, 256);
        expect(await s.hover(value)).toEqual(first);
    }
    await t.show("hover", s.hover(value));
    // Read again so the exit, which waits for the end of stderr, can be
    // seen.
    client.spawnStderrPump();
    await s.stop();
    const drained = client.drainedStderr().toString("utf8");
    expect(floodLines(drained)).toBeLessThan(BATCHES * LINES);
    // The mirror says where it dropped lines, however many each time. A
    // note may follow the part of a line the full pipe took.
    const gaps = drained
        .split("\n")
        .filter((line) => line.includes("[logging]"))
        .map((line) => line.slice(line.indexOf("[logging]")).replace(/\d+/g, "#"));
    await t.show("gap reports", [...new Set(gaps)]);
    const logged = t.disk
        .list(".clice/logs/**/master.log")
        .reduce((sum, log) => sum + floodLines(t.disk.read(log)), 0);
    expect(logged).toBe(BATCHES * LINES);
});
