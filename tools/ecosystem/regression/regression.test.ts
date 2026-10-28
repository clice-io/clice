/// Tests for the regression's steps: the units in flight when an index
/// stalls, the inputs a compiler check cannot have, and the sweep's places
/// and accepted differences.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { missingInputs } from "./confirm.ts";
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

describe("index", () => {
    test("the units in flight are those begun and not finished", () => {
        const log = [
            "[t] [info] [x] [1/3] Indexing /p/a.cpp",
            "[t] [info] [x] [2/3] Indexing /p/b.cpp",
            "[t] [info] [x] [perf:index] progress=1/3 file=/p/a.cpp bytes=1",
            "[t] [info] [x] [3/3] Indexing /p/c.cpp",
            "[t] [warning] [x] [3/3] Index failed for /p/c.cpp: no",
            "[t] [info] [x] [1/2] Indexing /p/d.cpp",
            "[t] [warning] [x] [1/2] Worker died while indexing /p/d.cpp; requeued: x",
            "[t] [info] [x] [2/2] Indexing /p/e.cpp",
            "[t] [warning] [x] [2/2] Index giving up on /p/e.cpp (crashed); its data stays",
            "[t] [info] [x] [1/1] Indexing /p/f.cpp",
            "[t] [info] [x] [1/1] Index dropped for removed file /p/f.cpp",
        ].join("\n");
        expect(unitsInFlight(log)).toEqual(["/p/b.cpp"]);
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

    test("a difference the project accepts is told apart from others", () => {
        const answer = (label: string) =>
            `["completion m2".completion]\nitems = [\n    {\n        label = "${label}",\n        kind = "Method",\n    },\n]\n`;
        const inspect = answer(
            "GetAsUInt64(uint64_t fail_value = UINT64_MAX, bool *success_ptr = nullptr) const",
        );
        const server = answer(
            "GetAsUInt64(uint64_t fail_value = 18446744073709551615ULL, bool *success_ptr = nullptr) const",
        );
        expect(acceptedDifference("completion", inspect, server)).toBe(
            "code_completion/member_access/20_macro_default_arguments",
        );
        expect(acceptedDifference("signatureHelp", inspect, server)).toBeUndefined();
        expect(
            acceptedDifference(
                "hover",
                "  uint32_t m_stop_id = UINT32_MAX;\n",
                "  uint32_t m_stop_id = 4294967295U;\n",
            ),
        ).toBe("code_completion/member_access/20_macro_default_arguments");
        expect(
            acceptedDifference("hover", "  int *p = nullptr;\n", "  int *p = 0;\n"),
        ).toBeUndefined();
        expect(acceptedDifference("hover", "  int n = 16;\n", "  int n = 32;\n")).toBeUndefined();
        expect(acceptedDifference("hover", "  int n = FOO;\n", "  int n = BAR;\n")).toBeUndefined();
        expect(
            acceptedDifference(
                "completion",
                inspect,
                answer("GetAsUInt64(uint64_t fail_value) const"),
            ),
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
