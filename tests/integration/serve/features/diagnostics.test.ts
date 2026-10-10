/// Published diagnostics: where each one lands in the document, the notes
/// it carries, and which diagnostics of other files reach a document.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve, ServeOptions } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

/// Every source a C++17 unit with `args`.
function cxx17(sources: string[], args: string[] = []): ServeOptions {
    return {
        manifest: {
            cxx: ["-std=c++17", ...args],
            units: Object.fromEntries(sources.map((source) => [source, []])),
        },
    };
}

function withCode(diagnostics: proto.Diagnostic[], code: string): proto.Diagnostic[] {
    return diagnostics.filter((diagnostic) => diagnostic.code === code);
}

function text(diagnostic: proto.Diagnostic): string {
    return typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value;
}

function span(range: proto.Range): string {
    return `${range.start.line}:${range.start.character}-${range.end.line}:${range.end.character}`;
}

/// Related information as `file@range message`, a workspace file by its
/// relative path.
function related(s: Serve, diagnostic: proto.Diagnostic): string[] {
    return (diagnostic.relatedInformation ?? []).map(
        (info) => `${s.relative(info.location.uri)}@${span(info.location.range)} ${info.message}`,
    );
}

serve.files(
    { "main.cpp": "int a = 1;\nint a = 2;\nvoid f(int);\nvoid g() { f(1, 2); }\n" },
    cxx17(["main.cpp"]),
)("notes become related information", async ({ s }) => {
    const diagnostics = await s.compiled("main.cpp");

    const [redefinition] = withCode(diagnostics, "err_redefinition");
    expect(span(redefinition!.range)).toBe("1:4-1:5");
    expect(related(s, redefinition!)).toEqual(["main.cpp@0:4-0:5 previous definition is here"]);
    const [call] = withCode(diagnostics, "err_ovl_no_viable_function_in_call");
    expect(related(s, call!)).toEqual([
        "main.cpp@2:5-2:6 candidate function not viable: requires 1 argument, but 2 were provided",
    ]);
});

serve.files(
    {
        "main.cpp":
            'double f() {\n    return 1.0 + "a";\n}\n[[nodiscard]] int value();\nvoid g() {\n    value();\n}\n',
    },
    cxx17(["main.cpp"]),
)("range holds the caret", async ({ s }) => {
    const diagnostics = await s.compiled("main.cpp");

    // Clang underlines both operands and puts the caret on the operator:
    // no range holds it, the caret's token stands.
    const [invalid] = withCode(diagnostics, "err_typecheck_invalid_operands");
    expect(span(invalid!.range)).toBe("1:15-1:16");
    // The caret starts the call clang underlines whole.
    const [discarded] = withCode(diagnostics, "warn_unused_result");
    expect(span(discarded!.range)).toBe("5:4-5:11");
});

serve.files(
    {
        "bad.h": "int one = undefined_one;\nint two = undefined_two;\n",
        // After code (the main parse reads bad.h) and in the preamble (the
        // PCH holds it, the error comes from the main parse all the same).
        "body.cpp": 'int a;\nint b;\n#include "bad.h"\nint c = undeclared_main;\n',
        "pre.cpp": '// one\n// two\n#include "bad.h"\nint c = undeclared_main;\n',
    },
    cxx17(["body.cpp", "pre.cpp"]),
)("header errors land on the include", async ({ s }) => {
    for (const file of ["body.cpp", "pre.cpp"]) {
        const diagnostics = await s.compiled(file);
        // One error per include line: the second error of bad.h is the
        // header's business, visible once bad.h itself is open.
        expect(
            diagnostics.map((diagnostic) => `${span(diagnostic.range)} ${text(diagnostic)}`),
        ).toEqual([
            "2:9-2:16 In included file: use of undeclared identifier 'undefined_one'",
            "3:8-3:23 use of undeclared identifier 'undeclared_main'",
        ]);
        expect(related(s, diagnostics[0]!)).toEqual(["bad.h@0:10-0:23 error occurred here"]);
    }
});

serve.files(
    {
        "box.h":
            "template <typename T>\nvoid touch(T t) { t.member(); }\n" +
            "template <typename T>\nstruct Box {\n    void put(T t) { touch(t); }\n};\n",
        "main.cpp": '#include "box.h"\nvoid f() {\n    Box<int> ints;\n    ints.put(1);\n}\n',
    },
    cxx17(["main.cpp"]),
)("instantiation errors land on the request", async ({ s }) => {
    const diagnostics = await s.compiled("main.cpp");

    const [error] = diagnostics;
    expect(diagnostics).toHaveLength(1);
    expect(span(error!.range)).toBe("3:9-3:12");
    expect(text(error!)).toBe(
        "In template: member reference base type 'int' is not a structure or union",
    );
    expect(related(s, error!)).toEqual([
        "box.h@1:19-1:20 error occurred here",
        "box.h@4:20-4:25 in instantiation of function template specialization 'touch<int>' requested here",
        "main.cpp@3:9-3:12 in instantiation of member function 'Box<int>::put' requested here",
    ]);
});

serve.files(
    {
        "a.h": "#pragma once\nint a = 1;\n",
        "w.h": "#pragma once\n",
        // The #ifdef on line 1 is never closed and sits inside the preamble.
        "main.cpp": '#include "a.h"\n#ifdef __linux__\n#include "w.h"\nint x = a;\n',
    },
    cxx17(["main.cpp"]),
)("preamble errors keep their place", async ({ s }) => {
    expect(
        (await s.compiled("main.cpp")).map(
            (diagnostic) => `${span(diagnostic.range)} ${diagnostic.code}`,
        ),
    ).toEqual(["1:1-1:6 err_pp_unterminated_conditional"]);
});

// Both files open with the same preamble and share its PCH; each sees the
// warnings of the PCH's build pointing into itself.
const PREAMBLE = '#define M 1\n#define M 2\n#pragma message("built")\n#include "c.h"\n';

serve.files(
    {
        "c.h": "#pragma once\nint c_val = 1;\n",
        "a.cpp": PREAMBLE + "int a = M;\n",
        "b.cpp": PREAMBLE + "int b = M;\n",
    },
    // Both builds raise the command line's warning; it appears once.
    cxx17(["a.cpp", "b.cpp"], ["-Wlogical-op"]),
)("preamble warnings are published", async ({ s }) => {
    for (const file of ["a.cpp", "b.cpp"]) {
        const diagnostics = await s.compiled(file);
        expect(
            diagnostics.map((diagnostic) => `${span(diagnostic.range)} ${diagnostic.code}`),
        ).toEqual([
            "1:8-1:9 ext_pp_macro_redef",
            "2:8-2:15 warn_pragma_message",
            "0:0-0:0 warn_unknown_diag_option",
        ]);
        expect(related(s, diagnostics[0]!)).toEqual([
            `${file}@0:8-0:9 previous definition is here`,
        ]);
    }
    expect(s.workspace.pchFiles()).toHaveLength(1);
});

serve.files(
    {
        // -Werror makes the unused variable an error, still the header's own.
        "redef.h":
            "#define LIMIT 1\n#define LIMIT 2\ninline int f() {\n    int unused = 0;\n    return 0;\n}\n",
        "main.cpp": 'int x = undeclared;\n#include "redef.h"\n',
    },
    cxx17(["main.cpp"], ["-Wall", "-Werror"]),
)("header warnings stay in the header", async ({ s }) => {
    // The redefinition and its "previous definition" note both stay out,
    // the note not attached to the error before them either.
    const diagnostics = await s.compiled("main.cpp");
    expect(diagnostics.map((diagnostic) => diagnostic.code)).toEqual(["err_undeclared_var_use"]);
    expect(diagnostics[0]!.relatedInformation).toBeUndefined();
});

serve.files(
    {
        "forced.h": "int forced = undeclared_forced;\n",
        "missing.cpp": "int a = 0;\n",
        "forced.cpp": "int b = 0;\n",
    },
    {
        manifest: {
            cxx: ["-std=c++17"],
            units: {
                "missing.cpp": ["-include", "missing.h"],
                "forced.cpp": ["-include", "forced.h"],
            },
        },
    },
)("command line includes report at the top", async ({ s }) => {
    expect(
        (await s.compiled("missing.cpp")).map(
            (diagnostic) => `${span(diagnostic.range)} ${text(diagnostic)}`,
        ),
    ).toEqual(["0:0-0:0 'missing.h' file not found"]);
    const diagnostics = await s.compiled("forced.cpp");
    expect(
        diagnostics.map((diagnostic) => `${span(diagnostic.range)} ${text(diagnostic)}`),
    ).toEqual(["0:0-0:0 In included file: use of undeclared identifier 'undeclared_forced'"]);
    expect(related(s, diagnostics[0]!)).toEqual(["forced.h@0:13-0:30 error occurred here"]);
});

serve.files(
    {
        "cmp.h": "template <typename T>\nbool less(T a, unsigned b) {\n    return a < b;\n}\n",
        "main.cpp": '#include "cmp.h"\nbool b = less(-1, 1u);\n',
    },
    cxx17(["main.cpp"], ["-Wsign-compare"]),
)("instantiation warnings land on the request", async ({ s }) => {
    // Not an error: published on the request as is, no prefix.
    expect(
        (await s.compiled("main.cpp")).map(
            (diagnostic) => `${span(diagnostic.range)} ${diagnostic.code} ${text(diagnostic)}`,
        ),
    ).toEqual([
        "1:9-1:13 warn_mixed_sign_comparison comparison of integers of different signs: 'int' and 'unsigned int'",
    ]);
});

serve.files(
    {
        "host.cpp":
            'struct S { int v; };\nint before_include = "x";\n#define FROM_HOST 1\n#include "frag.h"\n' +
            "int after_include = undeclared_after;\nint main() { return 0; }\n",
        "frag.h": "inline int use(S s) { return s.v + FROM_HOST; }\nint own = undeclared_own;\n",
    },
    cxx17(["host.cpp"]),
)("host errors stay out of headers", async ({ s }) => {
    expect(
        (await s.compiled("frag.h")).map(
            (diagnostic) => `${span(diagnostic.range)} ${diagnostic.code}`,
        ),
    ).toEqual(["1:10-1:24 err_undeclared_var_use"]);
});

serve.files(
    {
        // The broken include keeps the header out of a PCH, so the main
        // parse sees the #pragma once at its top.
        "src/hdr.h": '#pragma once\n#include "nothere.h"\n',
        // Its system-header pragma and static function are for its includers.
        "src/util.h":
            "#pragma once\n#pragma GCC system_header\nstatic inline int helper() { return 1; }\n",
        "src/mylib":
            "#pragma once\n#pragma GCC system_header\nstatic inline int other() { return 2; }\n",
        "src/user.cpp": '#include "hdr.h"\n#include "util.h"\n#include "mylib"\n',
    },
    cxx17(["src/user.cpp"], ["-Wall"]),
)("borrowed header stays a header", async ({ s }) => {
    expect((await s.compiled("src/hdr.h")).map((diagnostic) => diagnostic.code)).toEqual([
        "inferred-compile-command",
        "err_pp_file_not_found",
    ]);
    expect(await s.compiled("src/util.h")).toEqual([]);
    expect(await s.compiled("src/mylib")).toEqual([]);
});

serve.files(
    // /W3 is -Wall; a cl-mode driver reading back `-Wall` gets /Wall, every
    // warning there is (C++98 compatibility for each `auto`).
    { "main.cpp": "int main() {\n    auto x = 1;\n    return x;\n}\n" },
    { manifest: { units: {} } },
)("cl warning level stays", async ({ s }) => {
    // A manifest spells clang's driver: the cl entry is written while no
    // server runs.
    await s.offline(() => {
        s.disk.write(
            "compile_commands.json",
            JSON.stringify([
                {
                    directory: s.workspace.root,
                    file: s.workspace.path("main.cpp"),
                    arguments: [
                        "cl.exe",
                        "/nologo",
                        "/TP",
                        "/std:c++17",
                        "/W3",
                        "-c",
                        s.workspace.path("main.cpp"),
                    ],
                },
            ]),
        );
    });
    expect(await s.compiled("main.cpp")).toEqual([]);
});
