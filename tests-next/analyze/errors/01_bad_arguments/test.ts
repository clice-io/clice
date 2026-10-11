import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view module --module nowhere");
    await t.cli("analyze modules --view file --file nowhere.h");
    await t.cli("analyze modules --view nope");
    await t.cli("analyze modules --view edge --from core");
    await t.cli("analyze modules --scope [");
    await t.cli("analyze modules --merge core+nowhere");
    await t.cli("analyze modules --move nowhere.h=core");
    await t.cli("analyze modules --move-entity Nowhere=core/nowhere.h");
    await t.cli("analyze modules --partition nowhere.json");
    await t.cli("analyze modules --annotation nowhere.json");
    await t.cli("analyze modules --depth deep");
    await t.cli("analyze modules --configuration nowhere");
});
