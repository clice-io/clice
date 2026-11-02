import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// A response file's arguments take its place, a driver mode among them
// governing the rest; a UTF-8 byte order mark before them is no argument.
test("expanded", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ @flags.rsp main.cpp");
    await t.cli("command convert -- missing-clang++ @mode.rsp main.cpp");
    await t.cli("command convert -- missing-clang++ @bom.rsp main.cpp");
});
