import { expect } from "vitest";
import { test } from "@clice/tools/testing";

interface Edge {
    entities: { id: string; entity: string }[];
}

// A function of internal linkage is one per header including it: its
// locator names it with the header the index holds it in, and moved, it
// keeps that locator.
test(async (t) => {
    await t.cli("index");
    const used = await t.cli("analyze modules --view edge --from app --to core");
    const helper = (used.json as Edge).entities.find((entry) => entry.entity === "helper");
    expect(helper).toBeDefined();
    await t.cli([
        "analyze",
        "modules",
        "--move-entity",
        `${helper?.id ?? ""}=app/helper.h`,
        "--view",
        "edge",
        "--from",
        "core",
        "--to",
        "app",
    ]);
});
