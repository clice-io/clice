/// Tests for the regression's steps: the units in flight when an index
/// stalls, the inputs a compiler check cannot have, and the sweep's places
/// and accepted differences.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { compilerErrors, confirms, missingInputs } from "./confirm.ts";
import { unitsInFlight } from "./index.ts";
import { acceptedDifference, placesOf, shownPlace } from "./sweep.ts";

describe("confirm", () => {
    test("a command reading what only the build writes cannot be checked", () => {
        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-ecosystem-"));
        try {
            fs.writeFileSync(path.join(root, "built.pcm"), "");
            expect(
                missingInputs(
                    ["-fmodule-file=a=built.pcm", "-fmodule-file=b.pcm", "@a.o.modmap", "-I@x"],
                    root,
                ),
            ).toEqual(["b.pcm", "a.o.modmap"]);
        } finally {
            fs.rmSync(root, { recursive: true });
        }
    });
});

describe("compiler errors", () => {
    const dir = path.resolve("/build");
    const at = (file: string) => path.resolve(dir, file);

    test("the errors a compiler locates, in each compiler's form", () => {
        const gcc = [
            "../src/a.cpp: In function 'int main()':",
            "../src/a.cpp:3:5: error: 'foo' was not declared in this scope",
            "    3 |     foo();",
            "../src/b.h:7:10: fatal error: c.h: No such file or directory",
            "compilation terminated.",
        ].join("\n");
        expect(compilerErrors(gcc, dir)).toEqual([
            { file: at("../src/a.cpp"), line: 3, message: "'foo' was not declared in this scope" },
            { file: at("../src/b.h"), line: 7, message: "c.h: No such file or directory" },
        ]);
        const cl = [
            "a.cpp",
            "C:\\src\\a.cpp(3): error C3861: 'foo': identifier not found",
            "src/b.cpp(12,5): error: use of undeclared identifier 'bar'",
        ].join("\n");
        expect(compilerErrors(cl, dir).map(({ line, message }) => [line, message])).toEqual([
            [3, "'foo': identifier not found"],
            [12, "use of undeclared identifier 'bar'"],
        ]);
    });

    test("errors in color, as -fdiagnostics-color writes them", () => {
        const clang =
            "\x1b[1m../src/a.cpp:1:14: \x1b[0m\x1b[0;1;31merror: \x1b[0m\x1b[1munknown type name 'foo'\x1b[0m";
        expect(compilerErrors(clang, dir)).toEqual([
            { file: at("../src/a.cpp"), line: 1, message: "unknown type name 'foo'" },
        ]);
    });

    test("a driver's own errors locate none", () => {
        const driver = [
            "g++: error: unrecognized command-line option '-fno-such-thing'",
            "clang++: error: no such file or directory: 'missing.cpp'",
            "cl : Command line error D8021 : invalid numeric argument '/Wfoo'",
        ].join("\n");
        expect(compilerErrors(driver, dir)).toEqual([]);
    });

    test("the compiler confirms clice's first error by its line or its words", () => {
        const found = compilerErrors(
            "../src/a.cpp:3:5: error: 'foo' was not declared in this scope\n../src/a.cpp:9:1: error: expected ‘;’ before ‘}’ token",
            dir,
        );
        const clice = (line: number, message: string) => [
            { file: at("../src/a.cpp"), line, column: 5, message },
        ];
        expect(confirms(clice(3, "use of undeclared identifier 'foo'"), found)).toBe(true);
        expect(confirms(clice(5, "expected ';' before '}' token"), found)).toBe(true);
        expect(confirms(clice(5, "use of undeclared identifier 'foo'"), found)).toBe(false);
        expect(confirms([{ message: "expected ';' before '}' token" }], found)).toBe(true);
        expect(confirms([], found)).toBe(true);
    });
});

describe("index", () => {
    test("the units in flight are those begun and not finished", () => {
        const log = [
            "[t] [info] [1/4] Indexing a.cpp",
            "[t] [info] [2/4] Indexing b.cpp",
            "[t] [info] [1/4] Indexed a.cpp in 12 ms",
            "[t] [info] [3/4] Indexing c.cpp",
            "[t] [warning] [3/4] Cannot index c.cpp: no",
            "[t] [info] [4/4] Indexing g.cpp",
            "[t] [warning] [4/4] g.cpp does not fit in memory: its worker reached 9 MiB alone",
            "[t] [info] [1/2] Indexing d.cpp",
            "[t] [warning] [1/2] The worker indexing d.cpp died; it is indexed again later: x",
            "[t] [info] [2/2] Indexing e.cpp",
            "[t] [warning] [2/2] Not indexing e.cpp again (crashed); what other files know",
            "[t] [info] [1/1] Indexing f.cpp",
            "[t] [info] [1/1] f.cpp was removed while it was indexed",
            "[t] [info] [1/2] Indexing h.cpp",
            "[t] [info] [1/2] Indexing h.cpp again later: more urgent work came first",
            "[t] [info] [2/2] Indexing i.cpp",
            "[t] [info] [2/2] i.cpp or a file it includes changed while it was indexed; the result is dropped",
        ].join("\n");
        expect(unitsInFlight(log)).toEqual(["b.cpp"]);
    });
});

describe("sweep", () => {
    test("places: identifiers, member accesses, insides of calls", () => {
        const text = "#include <x>\nint f(int a) { return g(a).value; }\n";
        const places = placesOf("m.cpp", text);
        const words = places.points.map(
            (point) =>
                `${/\w*$/.exec(text.slice(0, point.begin))?.[0] ?? ""}${/^\w*/.exec(text.slice(point.begin))?.[0] ?? ""}`,
        );
        expect(words).toEqual(["f", "a", "g", "a", "value"]);
        expect(places.members.map((m) => text.slice(m.begin, m.begin + 5))).toEqual(["value"]);
        expect(places.calls.map((c) => text.slice(c.begin - 2, c.begin))).toEqual(["f(", "g("]);
    });

    test("places skip the names module declarations declare", () => {
        const text = "module;\n#include <x>\nexport module a.b:c;\nint module_x;\n";
        const words = placesOf("m.cppm", text).points.map(
            (point) =>
                `${/\w*$/.exec(text.slice(0, point.begin))?.[0] ?? ""}${/^\w*/.exec(text.slice(point.begin))?.[0] ?? ""}`,
        );
        expect(words).toEqual(["module", "module", "module_x"]);
    });

    test("a completion list cut short is accepted when inspect lists all it keeps", () => {
        const item = (label: string) => ({ label, kind: "Function", sortText: "1" });
        const answer = (labels: string[], incomplete: boolean) =>
            JSON.stringify({ items: labels.map(item), incomplete });
        const inspect = answer(["abort", "array", "atomic", "exit"], false);
        expect(acceptedDifference("completion", inspect, answer(["array", "atomic"], true))).toBe(
            "code_completion/filtering/08_item_limit",
        );
        expect(
            acceptedDifference("completion", inspect, answer(["array", "atomic"], false)),
        ).toBeUndefined();
        expect(
            acceptedDifference("completion", inspect, answer(["array", "vector"], true)),
        ).toBeUndefined();
        expect(
            acceptedDifference("completion", inspect, answer(["array", "array"], true)),
        ).toBeUndefined();
        expect(acceptedDifference("completion", inspect, answer([], true))).toBeUndefined();
        expect(
            acceptedDifference("signatureHelp", inspect, answer(["array"], true)),
        ).toBeUndefined();
    });

    test("a place is shown on its line, by its number where the line repeats", () => {
        const text = "int f();\nint g() {\n    return f();\n}\nint h() {\n    return f();\n}\n";
        const at = (begin: number) => ({
            path: "m.cpp",
            name: "p0",
            begin,
            end: begin,
            range: false,
            text,
        });
        expect(shownPlace(at(text.indexOf("f")))).toBe("int ⟦⟧f();");
        expect(shownPlace(at(text.lastIndexOf("f")))).toBe("return ⟦⟧f(); @6");
    });

    test("places spread over a file with more identifiers than points", () => {
        const text = Array.from({ length: 100 }, (_, i) => `int v${String(i)};\n`).join("");
        const points = placesOf("m.cpp", text).points.map((point) => point.begin);
        expect(points).toHaveLength(20);
        expect(points).toEqual([...new Set(points)].sort((a, b) => a - b));
        expect(points[0]).toBeLessThan(text.length / 20);
        expect(points.at(-1)).toBeGreaterThan((text.length * 19) / 20);
    });
});
