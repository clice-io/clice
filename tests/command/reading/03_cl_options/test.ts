import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// /Tc and /Tp pair a language with one file: each entry keeps its own
// file's, as the selector for every input, and drops the other file.
test("per-file languages", { errors: [MISSING] }, async (t) => {
    await t.cli("command alpha.c");
    await t.cli("command beta.c");
});

// cl spellings stay where cl's own reading of them differs from clang's
// (-Wall is cl's /Wall); a GCC-style option no cl spelling has goes
// through /clang:.
test("cl spellings", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli(
        "command convert -- cl-missing.exe /W3 -Wno-unused-variable /J /Zp4 /Iinc /FIpre.h /std:c++17 /c a.cpp",
    );
    await t.cli("command convert -- cl-missing.exe -std=c++20 /c b.cpp");
});
