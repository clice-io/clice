import { test } from "@clice/tools/testing";

// Each configuration has an index of its own: a query reads the one it
// names, else the default's.
test(async (t) => {
    await t.cli("index --configuration release");
    await t.cli("query --configuration release --method symbolSearch --symbol release_only");
    await t.cli("query --method symbolSearch --symbol debug_only");
    await t.cli("index");
    await t.cli("query --method symbolSearch --symbol debug_only");
    await t.cli("query --configuration nope --method symbolSearch --symbol debug_only");
});
