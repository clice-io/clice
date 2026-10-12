import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

// The PCH is damaged while the server is down, its size and time kept, so
// no freshness check notices. main.cpp has an error of its own: a file the
// damaged PCH bricked would publish nothing instead.

const PCH = ".clice/cache/*/pch/*.pch";

/// Damage the PCH offline; once the server is back, main.cpp reports its
/// own error again over a PCH built anew.
async function recovers(t: TestContext, s: Serve, where: "all" | "middle"): Promise<void> {
    await t.show("errors", s.errors("main.cpp"));
    const errors = await s.errors("main.cpp");
    const [damaged] = t.disk.list(PCH);
    await s.offline(() => {
        t.disk.corrupt(damaged ?? "", where);
    });
    expect(await s.errors("main.cpp")).toEqual(errors);
    await s.sync();
    expect((await s.counts()).pch).toBe(1);
    const rebuilt = t.disk.list(PCH);
    expect(rebuilt).toHaveLength(1);
    expect(rebuilt[0] === damaged).toBe(false);
}

test("damaged pch is rebuilt", async (t) => {
    // Garbage fails the reader's own checks: the pair is retracted at once.
    const s = await t.start();
    await recovers(t, s, "all");
});

test("damage past the checks", async (t) => {
    // PCHs carry no checksum: bytes flipped deep inside crash the compile
    // that reads them, retried once without clang-tidy, before the pair is
    // retracted.
    const s = await t.start({ crash: { request: "compile", file: "main.cpp", input: true } });
    await recovers(t, s, "middle");
    expect(await s.crashes()).toBe(2);
});
