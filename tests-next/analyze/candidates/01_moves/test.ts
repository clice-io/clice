import { test } from "@clice/tools/testing";

const SCOPE = "--scope core/**,util/**,lib/**,feature/**,app/**";

test(async (t) => {
    await t.cli("index");
    await t.cli(`analyze modules ${SCOPE}`);
    await t.cli(`analyze modules ${SCOPE} --limit 1`);
});
