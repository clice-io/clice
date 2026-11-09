import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A folder whose rules only edit flags waits to open, and already answers
// by the configuration it would open in: selecting it again asks for no
// restart.
test(async (t) => {
    const s = await t.start();
    await s.sync();
    expect(t.disk.list(".clice/**")).toEqual([]);
    await t.show("menu", s.configurations());
    expect(await s.switchConfiguration("debug")).toEqual({ success: true, reason: null });
    await s.sync();
    expect(s.restartRequests()).toEqual([]);
});
