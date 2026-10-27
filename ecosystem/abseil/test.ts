import { perf, regression, type PerfFile } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-22#6",
            why: "signature help names a parameter as the declaration does on one client and as the definition does on the other (libstdc++'s std::string::reserve)",
            configuration: "gcc",
            kind: "disagree",
            request: "signatureHelp",
        },
        {
            issue: "10-24#7",
            why: "in templates and macro arguments the open file's highlights hold more than the unit's own",
            kind: "disagree",
            request: "highlights",
            detail: /^(?![\s\S]*\n-\s+\{)/,
        },
        {
            issue: "10-21#17",
            why: "the macro naming an #include's header in the preamble gets no macro token from the server",
            kind: "disagree",
            request: "semanticTokens",
            file: "absl/debugging/stacktrace.cc",
            // That token missing, and no other difference.
            detail: /(?<![\s\S])(?=[\s\S]*^-\s+\{ at = "#include ⟦ABSL_STACKTRACE_INL_HEADER⟧", type = "macro" \},$)(?![\s\S]*^(?!--- |\+\+\+ )[-+](?!\s+\{ at = "#include ⟦ABSL_STACKTRACE_INL_HEADER⟧", type = "macro" \},$).*$)/m,
        },
        {
            issue: "10-24#6",
            why: "googletest builds with -WX, which makes clang's -Wcharacter-conversion (a warning cl does not have) an error",
            configuration: "msvc",
            kind: "compile",
            detail: /may change the meaning of the represented code unit/,
        },
    ],
});

/// A 5,000-line test, heavy with templates.
const rawHashSet: PerfFile = {
    file: "absl/container/internal/raw_hash_set_test.cc",
    symbol: {
        after: "TEST(GrowthInfoViewTest, GetGrowthLeft) {",
        at: "growth_info->⟦InitGrowthLeftNoDeleted⟧(/*growth_left=*/5, kCapacity);",
    },
    call: {
        after: "TEST(GrowthInfoViewTest, GetGrowthLeft) {",
        at: "growth_info->InitGrowthLeftNoDeleted(⟦⟧/*growth_left=*/5, kCapacity);",
    },
    member: {
        after: "TEST(GrowthInfoViewTest, GetGrowthLeft) {",
        at: "EXPECT_EQ(growth_info->⟦⟧GetGrowthLeftLowerBound(), 5);",
    },
    unqualified: {
        after: "TEST(GrowthInfoViewTest, GetGrowthLeft) {",
        at: "Gro⟦⟧wthInfoAllocator growth_info(kCapacity);",
    },
    template: "return c.⟦⟧common().TombstonesCount();",
};

perf("editing", { configuration: "clang" }, async (p) => {
    await p.coldStart(rawHashSet, { runs: 5 });
    await p.index(rawHashSet);
    await p.warmStart(rawHashSet, { runs: 5 });
    await p.editLoop(rawHashSet, { edits: 20 });
    await p.warmRequests(rawHashSet, { repeats: 30 });
});

perf("sessions", { configuration: "clang" }, async (p) => {
    const files = await p.pick({ sources: 20, headers: 10 });
    await p.index(rawHashSet);
    await p.session([...files.slice(0, 10), ...files.slice(20, 25)]);
    await p.sample(files);
    await p.references(
        "absl/strings/str_cat.h",
        "[[nodiscard]] std::string ⟦StrCat⟧(const AlphaNum& a, const AlphaNum& b);",
    );
    await p.references("absl/status/status.h", "ABSL_MUST_USE_RESULT bool ⟦ok⟧() const;");
});

perf("endurance", { configuration: "clang" }, async (p) => {
    const files = await p.pick({ sources: 20, headers: 10 });
    await p.index(rawHashSet);
    await p.soak(rawHashSet, { edits: 300 });
    await p.idle(rawHashSet, { rounds: 15 });
    await p.long([...files.slice(0, 10), ...files.slice(20, 25)], { minutes: 45 });
});

perf("index-load", { configuration: "clang" }, async (p) => {
    await p.duringIndex(rawHashSet, { minutes: 30 });
    await p.index(rawHashSet, { hold: 180 });
});

perf(
    "limited",
    {
        configuration: "clang",
        platforms: ["linux"],
        known: [
            {
                issue: "10-23#25",
                why: "under 1000 MB the background index stalls, waiting for headroom no running work frees",
                kind: "hang",
                request: "limitedIndex",
                file: "absl/container/internal/raw_hash_set_test.cc, 1000 MB",
            },
        ],
    },
    async (p) => {
        await p.limitedIndex(rawHashSet, { mb: 1500 });
        await p.limitedIndex(rawHashSet, { mb: 1000 });
    },
);

perf("msvc", { configuration: "msvc" }, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index(rawHashSet);
    await p.session(files);
    await p.sample(files);
});
