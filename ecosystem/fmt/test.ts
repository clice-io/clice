import { perf, regression, type PerfFile } from "@clice/tools/ecosystem";

regression();

const formatTest: PerfFile = {
    file: "test/format-test.cc",
    symbol: 'auto u = fmt::detail::⟦utf8_to_utf16⟧("лошадка");',
    call: 'auto u = fmt::detail::utf8_to_utf16(⟦⟧"лошадка");',
    member: 'EXPECT_EQ(L"\\x043B\\x043E\\x0448\\x0430\\x0434\\x043A\\x0430", u.⟦⟧str());',
    unqualified: "EXPECT_TRUE(isf⟦⟧inite(Float(0.0)));",
    template: "EXPECT_TRUE(isfinite(Float(fmt::detail::⟦⟧max_value<double>())));",
};

perf("editing", { configuration: "clang" }, async (p) => {
    await p.coldStart(formatTest, { runs: 5 });
    await p.index(formatTest);
    await p.warmStart(formatTest, { runs: 5 });
    await p.editLoop(formatTest, { edits: 20 });
    await p.warmRequests(formatTest, { repeats: 30 });
});

perf("sessions", { configuration: "clang" }, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index(formatTest);
    await p.session(files);
    await p.sample(files);
    await p.duringIndex(formatTest, { minutes: 30 });
});

perf("msvc", { configuration: "msvc" }, async (p) => {
    await p.index(formatTest);
});
