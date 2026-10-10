/// clang-tidy on open files: the nearest .clang-tidy's checks minus the
/// editor's exclusions, NOLINT, fixes through code actions, configuration
/// edits, and a crashing check.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

function text(diagnostic: proto.Diagnostic): string {
    return typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value;
}

/// The diagnostics of `source` as `line:code`.
function findings(diagnostics: proto.Diagnostic[], source = "clang-tidy"): string[] {
    return diagnostics
        .filter((diagnostic) => diagnostic.source === source)
        .map((diagnostic) => `${diagnostic.range.start.line}:${String(diagnostic.code)}`);
}

function quickFixes(reply: (proto.Command | proto.CodeAction)[] | null): proto.CodeAction[] {
    return (reply ?? []).filter(
        (item): item is proto.CodeAction => "kind" in item && item.kind === "quickfix",
    );
}

serve.files({
    ".clang-tidy": 'Checks: "-*,bugprone-integer-division"\n',
    "main.cpp": "double ratio(int a, int b) { return a / b; }\n",
})("findings name their check", async ({ s }) => {
    const finding = (await s.compiled("main.cpp")).find((d) => d.source === "clang-tidy");
    expect(finding?.code).toBe("bugprone-integer-division");
    expect(finding?.codeDescription?.href).toBe(
        "https://clang.llvm.org/extra/clang-tidy/checks/bugprone/integer-division.html",
    );
    expect(text(finding!)).not.toContain("[bugprone-integer-division]");
    expect(finding?.range.start).toEqual({ line: 0, character: 36 });
});

serve.files({ "main.cpp": "namespace n {}\nnamespace alias = n;\n" })(
    "defaults apply without configuration",
    async ({ s }) => {
        const diagnostics = await s.compiled("main.cpp");
        expect(findings(diagnostics)).toEqual(["1:misc-unused-alias-decls"]);
        expect(diagnostics[0]?.tags).toEqual([1]);
    },
);

serve.files({
    ".clang-tidy":
        'Checks: "-*,bugprone-integer-division,bugprone-use-after-move,misc-const-correctness"\n',
    "main.cpp":
        "namespace std {\n" +
        "template <class T> T&& move(T& t) { return static_cast<T&&>(t); }\n" +
        "}\n" +
        "struct S { int v; };\n" +
        "int use(S s) { S t = std::move(s); return s.v + t.v; }\n" +
        "double ratio(int a, int b) { return a / b; }\n",
})("unusable and slow checks stay off", async ({ s }) => {
    expect(findings(await s.compiled("main.cpp"))).toEqual(["5:bugprone-integer-division"]);
});

serve.files(
    {
        ".clang-tidy": 'Checks: "-*,bugprone-integer-division"\n',
        "main.cpp":
            "double r1(int a, int b) { return a / b; } // NOLINT\n" +
            "// NOLINTNEXTLINE(bugprone-integer-division)\n" +
            "double r2(int a, int b) { return a / b; }\n" +
            "double r3(int a, int b) { return a / b; }\n" +
            "int f() { int unused; return 0; } // NOLINT\n" +
            "int g() { int unused; return 0; }\n",
    },
    { manifest: { cxx: ["-std=c++23"], units: { "main.cpp": ["-Wunused-variable"] } } },
)("NOLINT silences findings and warnings", async ({ s }) => {
    const diagnostics = await s.compiled("main.cpp");
    expect(findings(diagnostics)).toEqual(["3:bugprone-integer-division"]);
    expect(findings(diagnostics, "clang")).toEqual(["5:warn_unused_variable"]);
});

serve.files({
    ".clang-tidy": 'Checks: "-*,modernize-use-nullptr"\n',
    "main.cpp": "int* p = 0;\n",
})("a fix applies through a code action", async ({ s }) => {
    const [finding] = await s.compiled("main.cpp");
    expect(finding?.code).toBe("modernize-use-nullptr");
    const [fix] = quickFixes(
        await s.codeActions(at("main.cpp", "= |0"), { range: finding!.range }),
    );
    expect(fix?.title).toBe("change '0' to 'nullptr'");
    const fixed = await s.apply(fix!.edit!);
    expect(fixed["main.cpp"]).toBe("int* p = nullptr;\n");

    expect(findings(await s.compiled("main.cpp"))).toEqual([]);
});

serve.files({
    ".clang-tidy": 'Checks: "-*,readability-duplicate-include"\n',
    "a.h": "#pragma once\n",
    "main.cpp": '#if 0\n#include "a.h"\n#endif\n#include "a.h"\nint x;\n#include "a.h"\n',
})("preamble includes reach the checks", async ({ s }) => {
    // The include past the preamble repeats the preamble's active one;
    // underlined as the fix removes it, the newline ending line 4 first.
    const diagnostics = await s.compiled("main.cpp");
    expect(findings(diagnostics)).toEqual(["4:readability-duplicate-include"]);
    const [finding] = diagnostics;
    expect(finding?.range.end).toEqual({ line: 5, character: 14 });
    const [fix] = quickFixes(
        await s.codeActions(at("main.cpp", 'int x;\n|#include "a.h"'), { range: finding!.range }),
    );
    expect((await s.apply(fix!.edit!))["main.cpp"]).toBe(
        '#if 0\n#include "a.h"\n#endif\n#include "a.h"\nint x;\n',
    );
});

serve.files({
    ".clang-tidy":
        'Checks: "-*,modernize-make-unique"\n' +
        "CheckOptions:\n" +
        "  modernize-make-unique.MakeSmartPtrFunction: 'mem::make_unique'\n" +
        "  modernize-make-unique.MakeSmartPtrFunctionHeader: 'mem.h'\n",
    "mem.h":
        "#pragma once\n" +
        "namespace std {\n" +
        "template <class T> struct default_delete {};\n" +
        "template <class T, class D = default_delete<T>> struct unique_ptr {\n" +
        "    explicit unique_ptr(T*);\n" +
        "    ~unique_ptr();\n" +
        "};\n" +
        "}\n",
    "main.cpp":
        '#include "mem.h"\nstd::unique_ptr<int> make() { return std::unique_ptr<int>(new int(1)); }\n',
})("an inserted include the preamble has is left out", async ({ s }) => {
    const [finding] = await s.compiled("main.cpp");
    expect(finding?.code).toBe("modernize-make-unique");
    const [fix] = quickFixes(
        await s.codeActions(at("main.cpp", "return |std::unique_ptr"), { range: finding!.range }),
    );
    expect((await s.apply(fix!.edit!))["main.cpp"]).toBe(
        '#include "mem.h"\nstd::unique_ptr<int> make() { return mem::make_unique<int>(1); }\n',
    );
});

serve.files({ "src/main.cpp": "int* p = 0;\n" })(
    "configuration edits are picked up",
    async ({ s }) => {
        expect(findings(await s.compiled("src/main.cpp"))).toEqual([]);

        // Created where the lookup found none, then edited — to another
        // size, which the look at the disk sees at once.
        s.disk.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
        await s.sync({ poll: true });
        expect(findings(await s.diagnostics("src/main.cpp"))).toEqual(["0:modernize-use-nullptr"]);

        s.disk.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
        await s.sync({ poll: true });
        expect(findings(await s.diagnostics("src/main.cpp"))).toEqual([]);
    },
);

serve.files({
    "clice.toml": "[diagnostics]\nclang_tidy = false\n",
    ".clang-tidy": 'Checks: "-*,bugprone-integer-division"\n',
    "main.cpp": "double ratio(int a, int b) { return a / b; }\n",
})("off by configuration", async ({ s }) => {
    expect(findings(await s.compiled("main.cpp"))).toEqual([]);
});

/// The crash notes on the file once the server settled; the master logs
/// a dead worker's last stderr before it acts on the death.
async function crashNotes(s: Serve, file: string): Promise<string[]> {
    await s.sync();
    return (await s.diagnostics(file))
        .map(text)
        .filter((message) => message.includes("clice's worker crashed"));
}

serve.files(
    {
        ".clang-tidy": 'Checks: "-*,bugprone-integer-division"\n',
        "main.cpp":
            "// tidy poison\ndouble ratio(int a, int b) { return a / b; }\nint broken = nullptr;\n",
    },
    { anomalies: true, env: { CLICE_TEST_TIDY_CRASH: "tidy poison" } },
)("a crashing check pauses only clang-tidy", async ({ s }) => {
    s.open("main.cpp");
    expect(await s.hover(at("main.cpp", "double r|atio"))).not.toBeNull();

    expect(await crashNotes(s, "main.cpp")).toEqual([
        expect.stringContaining("while running clang-tidy on this file"),
    ]);
    const diagnostics = await s.diagnostics("main.cpp");
    expect(findings(diagnostics)).toEqual([]);
    expect(findings(diagnostics, "clang")).toEqual(["2:err_init_conversion_failed"]);
    expect(s.workspace.workerCrashes(`compile ${s.workspace.displayPath("main.cpp")}`)).toBe(1);
});

serve.files(
    {
        ".clang-tidy": 'Checks: "-*,bugprone-integer-division"\n',
        "main.cpp": "int add(int a, int b) { return a + b; }\n#pragma clang __debug crash\n",
    },
    { anomalies: true, env: { CLICE_TEST_PRAGMA_CRASH: "1" } },
)("a compile crash acquits clang-tidy", async ({ s }) => {
    s.open("main.cpp");
    const add = at("main.cpp", "int a|dd(");
    expect(await s.hover(add)).toBeNull();

    expect(await crashNotes(s, "main.cpp")).toEqual([
        expect.stringContaining("while compiling this file"),
    ]);
    const compile = `compile ${s.workspace.displayPath("main.cpp")}`;
    expect(s.workspace.workerCrashes(compile)).toBe(2);

    // The retry a save grants runs without the pass it acquitted.
    s.save("main.cpp");
    expect(await s.hover(add)).toBeNull();
    await s.sync();
    expect(s.workspace.workerCrashes(compile)).toBe(3);
});
