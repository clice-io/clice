import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { missingInputs } from "./confirm.ts";
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
    test("a checking command reads a CMake precompiled header from its source", () => {
        expect(
            syntaxOnly({
                directory: "/b",
                file: "a.cpp",
                arguments: [
                    "clang++",
                    "-Xclang",
                    "-include-pch",
                    "-Xclang",
                    "/b/cmake_pch.hxx.pch",
                    "-Xclang",
                    "-include",
                    "-Xclang",
                    "/b/cmake_pch.hxx",
                    "-c",
                    "a.cpp",
                ],
            }),
        ).toEqual([
            "clang++",
            "-Xclang",
            "-include",
            "-Xclang",
            "/b/cmake_pch.hxx",
            "a.cpp",
            "-fsyntax-only",
        ]);
    });

    test("a checking command writes nothing", () => {
        expect(
            syntaxOnly({
                directory: "/b",
                file: "a.c",
                command: "clang -Wp,-MMD,a.o.d -Iinc -MD -MF a.d -MT a.o -c a.c -o a.o",
            }),
        ).toEqual(["clang", "-Iinc", "a.c", "-fsyntax-only"]);
    });

    test("a command reading what only the build writes cannot be checked", () => {
        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-nightly-"));
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
            expect(fs.readFileSync(path.join(root, "clice.toml"), "utf8")).toBe(
                '[[rules]]\ncompile_commands = [".nightly/compile_commands.json"]\n',
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
        expect(sampleFiles(units, 10, "s")).toEqual(["a.cpp", "b.cc", "d.c", "e.cpp"]);
        const many = Array.from({ length: 100 }, (_, i) => `f${String(i).padStart(3, "0")}.cpp`);
        const picked = sampleFiles(many, 5, "2026-10-17 x");
        expect(sampleFiles(many, 5, "2026-10-17 x")).toEqual(picked);
        expect(picked).toHaveLength(5);
        expect(sampleFiles(many, 5, "2026-10-18 x")).not.toEqual(picked);
        expect(picked).not.toEqual(many.slice(0, 5));
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

    test("places spread over a file with more identifiers than points", () => {
        const text = Array.from({ length: 100 }, (_, i) => `int v${String(i)};\n`).join("");
        const points = placesOf("m.cpp", text).points.map((point) => point.begin);
        expect(points).toHaveLength(20);
        expect(points).toEqual([...new Set(points)].sort((a, b) => a - b));
        expect(points[0]).toBeLessThan(text.length / 20);
        expect(points.at(-1)).toBeGreaterThan((text.length * 19) / 20);
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

describe("report", () => {
    test("failures with their reproduction, numbers beside the previous night's", () => {
        const index = {
            seconds: 120,
            units: 3,
            shards: 4,
            shardBytes: 1048576,
            symbols: 9,
            cacheBytes: 2097152,
            memory: { root: 10, child: 20, total: 30 },
            compiledWithErrors: 1,
            failed: 0,
        };
        const results: Results = {
            job: "p",
            platform: "linux",
            about: "v1",
            commit: "c",
            clice: "k",
            seed: "s",
            compilerRejects: [],
            logErrors: [],
            failures: [
                { kind: "compile", file: "a.cpp", detail: "a.cpp:1:1: boom", repro: "run it" },
            ],
            index,
        };
        const text = report(results, {
            ...results,
            failures: [],
            index: { ...index, seconds: 90 },
        });
        expect(text).toContain("# p on linux: 1 failures");
        expect(text).toContain("| index | 2.0 min | 90 s |");
        expect(text).toContain("## Units clice reports errors on, which the compiler compiles (1)");
        expect(text).toContain("run it");
    });

    test("one cause at many places is shown once", () => {
        const crash = (file: string) => ({
            kind: "crash" as const,
            file,
            request: "hover p0",
            detail: "clice inspect exited SIGKILL",
            repro: `run ${file}`,
        });
        const text = report({
            job: "p",
            platform: "linux",
            about: "v1",
            commit: "c",
            clice: "k",
            seed: "s",
            compilerRejects: [],
            logErrors: [],
            failures: [crash("a.c"), crash("b.c"), crash("c.c")],
        });
        expect(text).toContain("## Crashes (3)");
        expect(text).toContain("### a.c, hover p0 and 2 more\n\n- b.c, hover p0\n- c.c, hover p0");
        expect(text.match(/exited SIGKILL/g)).toHaveLength(1);
        expect(text).toContain("run a.c");
        expect(text).not.toContain("run b.c");
    });

    test("a detail holding a code block keeps its fence", () => {
        const results: Results = {
            job: "p",
            platform: "linux",
            about: "v1",
            commit: "c",
            clice: "k",
            seed: "s",
            compilerRejects: [],
            logErrors: [],
            failures: [
                {
                    kind: "disagree",
                    file: "a.cpp",
                    request: "hover p0",
                    detail: "-```cpp\n+```c\n",
                    repro: "run it",
                },
            ],
        };
        expect(report(results)).toContain("````\n-```cpp\n+```c\n\n````");
    });
});
