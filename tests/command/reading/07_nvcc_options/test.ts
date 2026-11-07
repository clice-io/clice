import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// nvcc's options, read by a table of their own: its spellings of clang's
// options become clang's, what only nvcc reads goes with its values, and
// what it passes its host compiler splits as the host splits its command
// line and reads as the host's options, cl's with a cl host. The commands
// name a CUDA installation no machine has, so clang's own driver, reading
// them, finds none on any.
test("nvcc options", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli(
        'command convert -- nvcc-missing -ccbin g++ --cuda-path=missing-cuda --gpu-architecture=sm_80 --std=c++17 --no-exceptions -Xptxas -O3 --dependency-output k.d -MD "-Xcompiler=-O2 -g" -c main.cu',
    );
    await t.cli(
        'command convert -- nvcc-missing -ccbin cl --cuda-path=missing-cuda "-Xcompiler= /EHsc /W4" -Xcompiler=-MDd,-Fdk.pdb,-FS -c main.cu',
    );
    await t.cli("command convert -- nvcc-missing -ccbin cl --cuda-path=missing-cuda -c kernel.cuh");
});
