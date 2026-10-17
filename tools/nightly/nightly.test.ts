import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { jobDatabase, syntaxOnly } from "./database.ts";
import { unitsInFlight } from "./index.ts";
import type { Results } from "./nightly.ts";
import { jobs, loadProjects, type Project } from "./projects.ts";
import { report } from "./report.ts";
import { placesOf, sampleFiles } from "./sweep.ts";

describe("projects", () => {
    test("the pinned projects load, a job per part", () => {
        expect(jobs(loadProjects()).map((job) => job.name)).toEqual([
            "llvm/llvm",
            "llvm/clang",
            "llvm/mlir",
            "llvm/lldb",
            "linux",
            "clice",
            "abseil",
            "mp-units",
        ]);
    });
});

describe("database", () => {
    test("a checking command writes nothing", () => {
        expect(
            syntaxOnly({
                directory: "/b",
                file: "a.c",
                command: "clang -Wp,-MMD,a.o.d -Iinc -MD -MF a.d -MT a.o -c a.c -o a.o",
            }),
        ).toEqual(["clang", "-Iinc", "a.c", "-fsyntax-only"]);
    });

    test("a part takes the units under its prefixes", () => {
        const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-nightly-")));
        try {
            const entries = ["one/a.cpp", "two/b.cpp", "two/c.cpp"].map((file) => ({
                directory: root,
                file: path.join(root, file),
                arguments: ["clang++", "-c", file],
            }));
            fs.writeFileSync(path.join(root, "db.json"), JSON.stringify(entries));
            const project: Project = {
                name: "p",
                about: "",
                prepare: [],
                database: "db.json",
                parts: { one: ["one/"], two: ["two/"] },
                sample: 1,
            };
            const own = jobDatabase({ name: "p/two", project, part: "two" }, root);
            expect(own.map((entry) => path.relative(root, entry.file))).toEqual([
                "two/b.cpp",
                "two/c.cpp",
            ]);
            expect(fs.readFileSync(path.join(root, "clice.toml"), "utf8")).toContain(
                ".nightly/compile_commands.json",
            );
            const uncovered = { ...project, parts: { one: ["one/"] } };
            expect(() =>
                jobDatabase({ name: "p/one", project: uncovered, part: "one" }, root),
            ).toThrow(/two\/b.cpp is in no part/);
        } finally {
            fs.rmSync(root, { recursive: true });
        }
    });
});

describe("sweep", () => {
    test("the seed picks the same sources", () => {
        const units = ["a.cpp", "b.cc", "c.S", "d.c", "e.cpp", "a.cpp"];
        const picked = sampleFiles(units, 3, "2026-10-17 x");
        expect(sampleFiles(units, 3, "2026-10-17 x")).toEqual(picked);
        expect(picked).toHaveLength(3);
        expect(picked).not.toContain("c.S");
    });

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
});

describe("index", () => {
    test("the units in flight are those begun and not finished", () => {
        const log = [
            "[t] [info] [x] [1/3] Indexing /p/a.cpp",
            "[t] [info] [x] [2/3] Indexing /p/b.cpp",
            "[t] [info] [x] [perf:index] progress=1/3 file=/p/a.cpp bytes=1",
            "[t] [info] [x] [3/3] Indexing /p/c.cpp",
            "[t] [warning] [x] [3/3] Index failed for /p/c.cpp: no",
        ].join("\n");
        expect(unitsInFlight(log)).toEqual(["/p/b.cpp"]);
    });
});

describe("report", () => {
    test("failures with their reproduction, numbers beside the previous night's", () => {
        const results: Results = {
            job: "p",
            about: "v1",
            commit: "c",
            clice: "k",
            seed: "s",
            compilerRejects: [],
            logErrors: [],
            failures: [
                { kind: "compile", file: "a.cpp", detail: "a.cpp:1:1: boom", repro: "run it" },
            ],
            index: {
                seconds: 120,
                units: 3,
                shards: 4,
                shardBytes: 1048576,
                symbols: 9,
                cacheBytes: 2097152,
                memory: { root: 10, child: 20, total: 30 },
                compiledWithErrors: 1,
                failed: 0,
            },
        };
        const text = report(results, { ...results, failures: [] });
        expect(text).toContain("# p: 1 failures");
        expect(text).toContain("| index | 2.0 min | 2.0 min |");
        expect(text).toContain("## Units clice reports errors on, which the compiler compiles (1)");
        expect(text).toContain("run it");
    });
});
