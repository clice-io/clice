import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// Relative include directories are written out from where the compile
// runs: the current directory, or the one -working-directory names; an
// absolute one stays as it is.
test("include directories", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -Iinclude -isystem sys/inc -iquote ../src main.cpp",
    );
    await t.cli([
        "command",
        "convert",
        "--",
        "missing-clang++",
        `-I${t.root}/vendor/include`,
        "main.cpp",
    ]);
    await t.cli("command convert -- missing-clang++ -working-directory build -Iinclude main.cpp");
});

// A leading = names the sysroot, which clang substitutes; the files an
// option reads are anchored, a bare --config name is clang's to find.
test("sysroot", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli([
        "command",
        "convert",
        "--",
        "missing-clang++",
        `--sysroot=${t.root}/sdk`,
        "-I=/usr/include/foo",
        "main.cpp",
    ]);
    await t.cli(
        "command convert -- missing-clang++ -fsanitize-ignorelist=ignore.txt --config=x.cfg --sysroot sdk main.cpp",
    );
});
