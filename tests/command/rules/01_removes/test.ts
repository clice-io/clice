import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// A removed flag matches however the command spells it, joined or apart;
// `*` stands for every value.
test("macros", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ --output=main.o -D A -D B=0 joined.cpp");
    await t.cli("command convert -- missing-clang++ --output=main.o -D A -D B=0 separate.cpp");
    await t.cli("command convert -- missing-clang++ --output=main.o -D A -D B=0 both.cpp");
    await t.cli(
        "command convert -- missing-clang++ --output=main.o -D A -D B=0 wildcard_joined.cpp",
    );
    await t.cli(
        "command convert -- missing-clang++ --output=main.o -D A -D B=0 wildcard_separate.cpp",
    );
});

// A flag written without its value, or with `*` for it, removes every
// value; a `*` joined to the name removes every flag of that prefix and
// kind, so the preprocessor's -Wp stays. One that takes several values
// removes nothing until they are written. `*` for a path removes every
// directory instead of naming a file in the rule's directory.
test("every value", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang -include pre.h -Iinc -Wall -Wextra -Wno-unused -Wp,-DKEPT -DKEEP every.c",
    );
    await t.cli("command convert -- missing-clang++ -Iinc -I/opt/include includes_joined.cpp");
    await t.cli("command convert -- missing-clang++ -Iinc -I/opt/include includes_separate.cpp");
});
