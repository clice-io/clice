import { test } from "@clice/tools/testing";

const SCOPE = "--scope core/** --scope util/** --scope lib/** --scope feature/** --scope app/**";

test(async (t) => {
    await t.cli("index");
    await t.cli(`analyze modules ${SCOPE}`);
    await t.cli(`analyze modules ${SCOPE} --limit 1`);
});
