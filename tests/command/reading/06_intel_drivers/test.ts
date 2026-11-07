import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// icpx takes GCC's options, icx-cl and dpcpp-cl cl's (icx too, on
// Windows): a cl option of theirs stays an option, not an input.
test("intel drivers", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-icpx -fsycl -O2 -DNDEBUG -c main.cpp");
    await t.cli(
        "command convert -- missing-icx-cl /nologo /TP /DWIN32 /EHsc /Od -MDd -fsycl /c main.cpp",
    );
    await t.cli("command convert -- missing-dpcpp-cl /DWIN32 /EHsc -fsycl /c main.cpp");
});
