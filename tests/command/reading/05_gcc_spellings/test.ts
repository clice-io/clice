import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// A cross GCC that is not here leaves its command to clang's own driver,
// shown as that driver reads it: for the target the compiler's name sets,
// GCC's spellings as clang takes them, and without the names clang's
// target does not know.
test("cross gcc", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- riscv32-wch-elf-gcc -march=rv32imc_zba_xw -mabi=ilp32 -msmall-data-limit=8 -c main.c",
    );
    await t.cli(
        "command convert -- aarch64-none-elf-gcc -mcpu=cortex-a72.cortex-a53 -mabi=lp64 -c main.c",
    );
});

// GCC's -fmodules is its C++20 modules, which clang's C++20 has: to clang
// it would mean header modules.
test("gcc modules", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-g++ -std=c++20 -fmodules -c main.cpp");
});
