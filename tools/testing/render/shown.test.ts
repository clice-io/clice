import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { block, shown } from "./shown.ts";

const plain = { parts: new Set<string>(), full: false };
const root = fs.realpathSync.native(os.tmpdir());

test("text shows as it is", async () => {
    const text = `a ${path.join(root, "x.cpp")}\nb`;
    expect(await block(text, plain, root, "t")).toEqual({ value: "a ${WS}/x.cpp\nb" });
    expect(await block(3, plain, root, "t")).toEqual({ value: 3 });
    expect(await block(["x", "y"], plain, root, "t")).toEqual({ value: ["x", "y"] });
});

test("a request shows under its kind", async () => {
    const reply = shown(Promise.resolve([1, 2, 3]), "items", (items, { parts, full }) => [
        `${String(full ? items.length : 1)} of ${String(items.length)}`,
        ...parts,
    ]);
    expect(await block(reply, { parts: new Set(["insert"]), full: false }, root, "t")).toEqual({
        items: ["1 of 3", "insert"],
    });
    expect(await block(reply, { ...plain, full: true }, root, "t")).toEqual({
        items: ["3 of 3"],
    });
    await expect(block(Promise.resolve(1), plain, root, "t")).rejects.toThrow(
        /no request of a client/,
    );
});

test("a record shows its entries under their labels", async () => {
    const reply = shown(Promise.resolve([]), "definition", () => []);
    expect(await block({ definition: reply, declaration: ["a", "b"] }, plain, root, "t")).toEqual({
        definition: [],
        declaration: ["a", "b"],
    });
    await expect(block({ wrong: "x" }, plain, root, "t")).rejects.toThrow(/known-wrong/);
});
