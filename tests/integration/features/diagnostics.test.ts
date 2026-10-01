/// Published diagnostics: where each one lands in the document, the notes
/// it carries, and which diagnostics of other files reach a document.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

function published(client: CliceClient, uri: string): proto.Diagnostic[] {
    return client.diagnostics.get(uri) ?? [];
}

function withCode(client: CliceClient, uri: string, code: string): proto.Diagnostic[] {
    return published(client, uri).filter((diagnostic) => diagnostic.code === code);
}

function span(range: proto.Range): string {
    return `${range.start.line}:${range.start.character}-${range.end.line}:${range.end.character}`;
}

/// Related information as `file@range message`, the file by its name.
function related(client: CliceClient, diagnostic: proto.Diagnostic): string[] {
    return (diagnostic.relatedInformation ?? []).map((info) => {
        const file = client.normalizeUri(info.location.uri).split("/").pop();
        return `${file}@${span(info.location.range)} ${info.message}`;
    });
}

test("notes become related information", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int a = 1;\nint a = 2;\nvoid f(int);\nvoid g() { f(1, 2); }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const [redefinition] = withCode(client, uri, "err_redefinition");
    expect(span(redefinition!.range)).toBe("1:4-1:5");
    expect(related(client, redefinition!)).toEqual([
        "main.cpp@0:4-0:5 previous definition is here",
    ]);
    const [call] = withCode(client, uri, "err_ovl_no_viable_function_in_call");
    expect(related(client, call!)).toEqual([
        "main.cpp@2:5-2:6 candidate function not viable: requires 1 argument, but 2 were provided",
    ]);
});

test("range holds the caret", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", 'double f() {\n    return 1.0 + "a";\n}\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    // Clang underlines both operands and puts the caret on the operator.
    const [invalid] = withCode(client, uri, "err_typecheck_invalid_operands");
    expect(span(invalid!.range)).toBe("1:15-1:16");
});

test("header errors land on the include", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("bad.h", "int one = undefined_one;\nint two = undefined_two;\n");
    // After code (the main parse reads bad.h) and in the preamble (the
    // PCH holds it, the error comes from the main parse all the same).
    workspace.write("body.cpp", 'int a;\nint b;\n#include "bad.h"\nint c = undeclared_main;\n');
    workspace.write("pre.cpp", '// one\n// two\n#include "bad.h"\nint c = undeclared_main;\n');
    workspace.writeCDB(["body.cpp", "pre.cpp"]);
    await client.initialize(workspace);

    for (const file of ["body.cpp", "pre.cpp"]) {
        const [uri] = await client.openAndWait(file);
        // One error per include line: the second error of bad.h is the
        // header's business, visible once bad.h itself is open.
        expect(
            published(client, uri).map(
                (diagnostic) => `${span(diagnostic.range)} ${diagnostic.message}`,
            ),
        ).toEqual([
            "2:9-2:16 In included file: use of undeclared identifier 'undefined_one'",
            "3:8-3:23 use of undeclared identifier 'undeclared_main'",
        ]);
        expect(related(client, published(client, uri)[0]!)).toEqual([
            "bad.h@0:10-0:23 error occurred here",
        ]);
    }
});

test("instantiation errors land on the request", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "box.h",
        "template <typename T>\nvoid touch(T t) { t.member(); }\n" +
            "template <typename T>\nstruct Box {\n    void put(T t) { touch(t); }\n};\n",
    );
    workspace.write(
        "main.cpp",
        '#include "box.h"\nvoid f() {\n    Box<int> ints;\n    ints.put(1);\n}\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const [error] = published(client, uri);
    expect(published(client, uri)).toHaveLength(1);
    expect(span(error!.range)).toBe("3:9-3:12");
    expect(error!.message).toBe(
        "In template: member reference base type 'int' is not a structure or union",
    );
    expect(related(client, error!)).toEqual([
        "box.h@1:19-1:20 error occurred here",
        "box.h@4:20-4:25 in instantiation of function template specialization 'touch<int>' requested here",
        "main.cpp@3:9-3:12 in instantiation of member function 'Box<int>::put' requested here",
    ]);
});

test("preamble errors keep their place", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("a.h", "#pragma once\nint a = 1;\n");
    workspace.write("w.h", "#pragma once\n");
    // The #ifdef on line 1 is never closed and sits inside the preamble.
    workspace.write("main.cpp", '#include "a.h"\n#ifdef __linux__\n#include "w.h"\nint x = a;\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const [unterminated] = withCode(client, uri, "err_pp_unterminated_conditional");
    expect(span(unterminated!.range)).toBe("1:1-1:6");
});

test("header warnings stay in the header", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("redef.h", "#define LIMIT 1\n#define LIMIT 2\n");
    workspace.write("main.cpp", 'int x = undeclared;\n#include "redef.h"\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    // The redefinition and its "previous definition" note both stay out,
    // the note not attached to the error before them either.
    const diagnostics = published(client, uri);
    expect(diagnostics.map((diagnostic) => diagnostic.code)).toEqual(["err_undeclared_var_use"]);
    expect(diagnostics[0]!.relatedInformation).toBeUndefined();
});

test("host errors stay out of headers", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "host.cpp",
        'struct S { int v; };\nint before_include = "x";\n#define FROM_HOST 1\n#include "frag.h"\n' +
            "int after_include = undeclared_after;\nint main() { return 0; }\n",
    );
    workspace.write(
        "frag.h",
        "inline int use(S s) { return s.v + FROM_HOST; }\nint own = undeclared_own;\n",
    );
    workspace.writeCDB(["host.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("frag.h");

    expect(
        published(client, uri).map((diagnostic) => `${span(diagnostic.range)} ${diagnostic.code}`),
    ).toEqual(["1:10-1:24 err_undeclared_var_use"]);
});
