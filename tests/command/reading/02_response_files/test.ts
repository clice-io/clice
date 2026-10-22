import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// A response file's arguments take its place, a driver mode among them
// governing the rest.
test("expanded", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ @flags.rsp main.cpp");
    await t.cli("command convert -- missing-clang++ @mode.rsp main.cpp");
});
