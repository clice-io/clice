import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The end of a pass counts the files it indexed and those that failed: a
// file whose run crashed its worker is not one it indexed.

const PROGRESS = "clice/backgroundIndex";

test(async (t) => {
    const s = await t.start({ crash: { request: "compileUnit", file: "poison.cpp" } });
    await s.sync();
    // The first pass may have ended while its token was created, its token
    // ending after the server went idle.
    const created = s.client.progressTokens.filter((token) => token === PROGRESS).length;
    await s.client.progressed(PROGRESS, "end", created);
    const values = () =>
        s.client.progressEvents
            .filter((event) => event.token === PROGRESS)
            .map((event) => event.value as { kind: string; message?: string });
    const count = (kind: string) => values().filter((value) => value.kind === kind).length;

    // The pass is held until the client sees it begin, so that its end
    // carries the counts.
    const begins = count("begin");
    const ends = count("end");
    const held = await s.park("index", "healthy.cpp", { at: "reply" });
    t.disk.edit("healthy.cpp", { replace: "return 1;", with: "return 3;" });
    t.disk.edit("poison.cpp", { replace: "return 2;", with: "return 4;" });
    expect(await s.poll("workspace")).toBe(2);
    await held.reached();
    await s.client.progressed(PROGRESS, "begin", begins + 1);
    await held.release();
    await s.client.progressed(PROGRESS, "end", ends + 1);
    await t.show("end", { message: values().at(-1)?.message });
});
