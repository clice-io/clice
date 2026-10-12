import { test } from "@clice/tools/testing";

// core and util name each other. A module merged away answers to the one it
// joined: meta joins util, which joins core.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --merge core+util");
    await t.cli("analyze modules --merge core+util,util+meta");
    await t.cli("analyze modules --partition partition.json");
});
