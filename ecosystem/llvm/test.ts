import { perf, regression, type PerfFile } from "@clice/tools/ecosystem";

regression({
    configurations: ["all"],
    known: [
        {
            issue: "10-22#7",
            why: "on the 7 GB macOS runners clice index gives up flang's largest units, over 4 GB each to compile alone",
            part: "flang",
            platform: "macos",
            kind: "memory",
        },
    ],
});

/// A 22,000-line source.
const semaExpr: PerfFile = {
    file: "clang/lib/Sema/SemaExpr.cpp",
    symbol: {
        after: "CK_FunctionToPointerDecay);",
        at: "Res = ⟦DefaultLvalueConversion⟧(Res.get());",
    },
    call: {
        after: "CK_FunctionToPointerDecay);",
        at: "Res = DefaultLvalueConversion(⟦⟧Res.get());",
    },
    member: {
        after: "ExprResult Sema::DefaultFunctionArrayConversion(Expr *E, bool Diagnose) {",
        at: "QualType Ty = E->⟦⟧getType();",
    },
    unqualified: {
        after: "CK_FunctionToPointerDecay);",
        at: "Res = Def⟦⟧aultLvalueConversion(Res.get());",
    },
    template: "int order = S.Context.⟦⟧getIntegerTypeOrder(LHSType, RHSType);",
};

/// A large header, compiled inside a unit including it.
const semaH: PerfFile = {
    file: "clang/include/clang/Sema/Sema.h",
    symbol: "ExprResult ⟦DefaultLvalueConversion⟧(Expr *E);",
    member: "if (!getLangOpts().⟦⟧CPlusPlus || E.isInvalid())",
    unqualified: "if (!get⟦⟧LangOpts().CPlusPlus || E.isInvalid())",
    call: {
        after: "if (Idx != UINT_MAX)",
        at: "Diag(⟦⟧getAttrLoc(AI), diag::err_attribute_argument_n_type)",
    },
};

/// A header nearly every unit includes.
const smallVector: PerfFile = {
    file: "llvm/include/llvm/ADT/SmallVector.h",
    symbol: "bool ⟦isSmall⟧() const { return this->BeginX == getFirstEl(); }",
    template: "this->⟦⟧BeginX = getFirstEl();",
    unqualified: "this->BeginX = get⟦⟧FirstEl();",
    call: "return isReferenceToRange(⟦⟧V, this->begin(), this->end());",
};

perf("index", { configuration: "clang" }, async (p) => {
    await p.index(semaExpr);
    await p.warmStart(semaExpr, { runs: 5 });
    await p.editLoop(semaExpr, { edits: 20 });
    await p.warmRequests(semaExpr, { repeats: 20 });
    for (const file of [semaH, smallVector]) {
        await p.warmStart(file, { runs: 3 });
        await p.editLoop(file, { edits: 10 });
        await p.warmRequests(file, { repeats: 10 });
    }
});

perf("cold", { configuration: "clang" }, async (p) => {
    await p.coldStart(semaExpr, { runs: 5 });
    await p.coldStart(semaH, { runs: 3 });
    await p.coldStart(smallVector, { runs: 3 });
});

perf("sessions", { configuration: "clang", platforms: ["linux"] }, async (p) => {
    const files = await p.pick({
        sources: 20,
        headers: 10,
        under: ["llvm/lib", "llvm/include", "clang/lib", "clang/include"],
    });
    await p.index(semaExpr);
    await p.session([...files.slice(0, 10), ...files.slice(20, 25)]);
    await p.sample(files);
});

perf("all", { configuration: "all", platforms: ["linux"] }, async (p) => {
    await p.sample(
        [
            "clang/lib/Sema/SemaExpr.cpp",
            "mlir/lib/IR/Operation.cpp",
            "llvm/include/llvm/ADT/SmallVector.h",
        ],
        { wipe: "cache" },
    );
    await p.scan({ runs: 20 });
});
