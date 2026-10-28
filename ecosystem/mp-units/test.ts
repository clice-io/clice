import { perf, regression, type PerfFile } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-28#4",
            why: "importing the module after a preamble that includes the same standard headers fails clang's check that their definitions agree",
            kind: "compile",
            file: "test/runtime/fixed_string_test.cpp",
            // That error alone, and why the compiler could not check the unit.
            detail: /(?<![\s\S])(?=[\s\S]*'std::basic_istream' has different definitions in different modules)(?![\s\S]*^(?!.*'std::basic_istream' has different definitions in different modules|the compiler could not check it: ).+$)/m,
        },
    ],
});

/// A test importing the library's module.
const mathTest: PerfFile = {
    file: "test/runtime/math_test.cpp",
    symbol: "REQUIRE(⟦sqrt⟧(4 * isq::area[m2]) == 2 * isq::length[m]);",
    call: "REQUIRE(sqrt(⟦⟧4 * isq::area[m2]) == 2 * isq::length[m]);",
    unqualified: "REQUIRE(sq⟦⟧rt(4 * isq::area[m2]) == 2 * isq::length[m]);",
};

perf("editing", { clangdArgs: ["--experimental-modules-support"] }, async (p) => {
    await p.coldStart(mathTest, { runs: 3 });
    await p.index(mathTest);
    await p.warmStart(mathTest, { runs: 3 });
    await p.editLoop(mathTest, { edits: 20 });
    await p.warmRequests(mathTest, { repeats: 30 });
});
