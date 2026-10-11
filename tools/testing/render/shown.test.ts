import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { render, shown } from "./shown.ts";

const plain = { parts: new Set<string>(), full: false };
const root = fs.realpathSync.native(os.tmpdir());

test("text shows as it is", async () => {
    const text = `a ${path.join(root, "x.cpp")}\nb`;
    expect(await render(text, plain, root, "t")).toEqual(["a ${WS}/x.cpp", "b"]);
    expect(await render(3, plain, root, "t")).toEqual(["3"]);
    expect(await render(["x", "y"], plain, root, "t")).toEqual(["x", "y"]);
});

test("a request renders itself", async () => {
    const reply = shown(Promise.resolve([1, 2, 3]), (items, { parts, full }) => [
        `${String(full ? items.length : 1)} of ${String(items.length)}`,
        ...parts,
    ]);
    expect(await render(reply, { parts: new Set(["insert"]), full: false }, root, "t")).toEqual([
        "1 of 3",
        "insert",
    ]);
    expect(await render(reply, { ...plain, full: true }, root, "t")).toEqual(["3 of 3"]);
    await expect(render(Promise.resolve(1), plain, root, "t")).rejects.toThrow(
        /no request of a client/,
    );
});

test("a record shows a block each", async () => {
    const reply = shown(Promise.resolve([]), () => []);
    expect(await render({ definition: reply, declaration: ["a", "b"] }, plain, root, "t")).toEqual([
        "definition: (none)",
        "declaration",
        "  a",
        "  b",
    ]);
});
