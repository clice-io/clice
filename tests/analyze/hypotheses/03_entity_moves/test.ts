import { expect } from "vitest";
import { test } from "@clice/tools/testing";

interface Edge {
    entities: { id: string; entity: string }[];
}

// core names util's Clock, util core's Str and length: with Clock declared
// in core the cycle is gone, and what the moved definition names moves
// with it. A locator the edge view prints picks the same declaration.
test("moved declarations", async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --move-entity Clock=core/clock.h");
    const edge = await t.cli("analyze modules --view edge --from core --to util");
    const clock = (edge.json as Edge).entities.find((entry) => entry.entity === "Clock");
    expect(clock).toBeDefined();
    await t.cli([
        "analyze",
        "modules",
        "--move-entity",
        `${clock?.id ?? ""}=core/clock.h`,
        "--view",
        "edge",
        "--from",
        "core",
        "--to",
        "util",
    ]);
    await t.cli(
        "analyze modules --move-entity Clock=meta/clock.h --view edge --from meta --to core",
    );
    await t.cli(
        "analyze modules --move-entity Clock=meta/clock.h --view edge --from core --to meta",
    );
});

// Each repetition is one more move.
test("repeated moves", async (t) => {
    await t.cli("index");
    await t.cli(
        "analyze modules --move-entity twice=core/twice.h --view edge --from core --to util",
    );
    await t.cli([
        "analyze",
        "modules",
        "--move-entity",
        "twice=core/twice.h",
        "--move-entity",
        "kind:struct,class Clock=core/clock.h",
        "--view",
        "edge",
        "--from",
        "core",
        "--to",
        "util",
    ]);
});
