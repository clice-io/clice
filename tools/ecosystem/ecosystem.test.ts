/// Tests for the ecosystem framework (ecosystem/README.md): the projects
/// and the jobs their scripts register, request positions, known issues,
/// preparation, a job's database, process-tree memory and the reports.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import {
    inPart,
    jobDatabase,
    sampleFiles,
    syntaxOnly,
    thinned,
    warningIsError,
} from "./database.ts";
import { parsed, residentNow, watchTree } from "./memory.ts";
import { prepare, withoutOrderOnly } from "./prepare.ts";
import { validated, type Configuration, type Project } from "./project.ts";
import type { Results } from "./regression/regression.ts";
import { regressionReport } from "./report.ts";
import { allJobs, findJob, knownIssue, positionOf, sortKnown, type Job } from "./script.ts";

function scratch(): string {
    return fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-ecosystem-")));
}

describe("projects", () => {
    test("every project's script registers its jobs, a regression job per part", async () => {
        const jobs = await allJobs();
        const named = (kind: Job["kind"]) =>
            jobs.filter((job) => job.kind === kind).map((job) => job.name);
        expect(named("regression")).toEqual([
            "abseil/clang",
            "abseil/gcc",
            "abseil/msvc",
            "clice",
            "cpython",
            "cuda",
            "curl",
            "eigen",
            "flameshot",
            "fmt/clang",
            "fmt/msvc",
            "linux",
            "llvm/all/llvm",
            "llvm/all/clang",
            "llvm/all/mlir",
            "llvm/all/flang",
            "llvm/all/lldb",
            "mp-units",
        ]);
        expect(named("perf")).toEqual([
            "abseil/editing",
            "abseil/sessions",
            "abseil/endurance",
            "abseil/index-load",
            "abseil/limited",
            "abseil/msvc",
            "cuda/sessions",
            "curl/sessions",
            "eigen/sessions",
            "flameshot/sessions",
            "fmt/editing",
            "fmt/sessions",
            "fmt/msvc",
            "linux/startup",
            "llvm/index",
            "llvm/cold",
            "llvm/sessions",
            "llvm/all",
            "mp-units/editing",
            "sqlite/amalgamation",
        ]);
        const gcc = jobs.find((job) => job.name === "abseil/gcc");
        expect(gcc?.platforms).toEqual(["linux"]);
        expect(gcc?.config.packages).toEqual(["g++-13"]);
    });

    test("a project.json that cannot be what it says is an error", () => {
        const project: Project = {
            name: "p",
            about: "",
            repository: "https://example.com/p",
            commit: "0123456789abcdef0123456789abcdef01234567",
            configurations: { c: { about: "", prepare: [], database: "db.json" } },
        };
        expect(validated(project)).toBe(project);
        const broken = (change: Partial<Project>) => () => validated({ ...project, ...change });
        expect(broken({ commit: "main" })).toThrow(/pins main, not a full commit hash/);
        expect(() => validated({ ...project, commit: undefined } as unknown as Project)).toThrow(
            /a repository without a commit/,
        );
        expect(broken({ archive: { url: "u", sha256: "0".repeat(64) } })).toThrow(
            /both a repository and an archive/,
        );
        expect(broken({ configurations: {} })).toThrow(/has no configuration/);
        const config = { about: "", prepare: [], database: "db.json" };
        expect(
            broken({ configurations: { c: { ...config, platforms: ["mac" as "macos"] } } }),
        ).toThrow(/c names unknown platforms: mac/);
        expect(
            broken({ configurations: { c: { ...config, parts: { a: ["a/"] }, keep: 1 } } }),
        ).toThrow(/c both splits its database and thins it/);
    });
});

describe("positions", () => {
    const root = scratch();
    fs.writeFileSync(
        path.join(root, "a.cpp"),
        [
            "int value = 1;",
            "TEST(First) {",
            "  check(value);",
            "}",
            "TEST(Second) {",
            "  check(value);",
            "}",
        ].join("\n"),
    );

    test("a line's code names its place, a repeated one the first after another", () => {
        expect(positionOf(root, "a.cpp", "int ⟦value⟧ = 1;")).toEqual({ line: 0, character: 4 });
        expect(positionOf(root, "a.cpp", "int value = ⟦⟧1;")).toEqual({ line: 0, character: 12 });
        expect(
            positionOf(root, "a.cpp", { after: "TEST(Second) {", at: "check(⟦value⟧);" }),
        ).toEqual({ line: 5, character: 8 });
    });

    test("a place naming no line, or several, is an error", () => {
        expect(() => positionOf(root, "a.cpp", "check(⟦value⟧);")).toThrow(/no unique range/);
        expect(() => positionOf(root, "a.cpp", "int ⟦other⟧ = 1;")).toThrow(/no unique range/);
        expect(() =>
            positionOf(root, "a.cpp", { after: "TEST(Second) {", at: "int ⟦value⟧ = 1;" }),
        ).toThrow(/no 'int ⟦value⟧ = 1;' after 'TEST\(Second\) \{'/);
    });
});

describe("known issues", () => {
    const job = {
        kind: "regression",
        name: "p/gcc",
        configuration: "gcc",
        platforms: ["linux"],
        sample: 1,
        known: [
            {
                issue: "10-22#6",
                why: "names differ",
                configuration: "gcc",
                kind: "disagree",
                request: "signatureHelp",
            },
            { issue: "10-22#7", why: "too big", platform: "macos", kind: "memory", file: /^big\// },
            { issue: "10-22#8", why: "flang's", part: "flang", kind: "hang" },
        ],
    } as unknown as Job;

    test("a failure is the known issue whose every field it matches", () => {
        const disagree = { kind: "disagree", detail: "", repro: "" };
        expect(
            knownIssue(job, "linux", { ...disagree, file: "a.cpp", request: "signatureHelp" })
                ?.issue,
        ).toBe("10-22#6");
        expect(knownIssue(job, "linux", { ...disagree, request: "hover" })).toBeUndefined();
        const memory = { kind: "memory", detail: "", repro: "" };
        expect(knownIssue(job, "macos", { ...memory, file: "big/x.cpp" })?.issue).toBe("10-22#7");
        expect(knownIssue(job, "linux", { ...memory, file: "big/x.cpp" })).toBeUndefined();
        expect(knownIssue(job, "macos", { ...memory, file: "small/x.cpp" })).toBeUndefined();
    });

    test("a known issue of one configuration or part matches no other", () => {
        const disagree = { kind: "disagree", request: "signatureHelp", detail: "", repro: "" };
        expect(knownIssue({ ...job, configuration: "clang" }, "linux", disagree)).toBeUndefined();
        const hang = { kind: "hang", detail: "", repro: "" };
        expect(knownIssue({ ...job, part: "flang" } as Job, "linux", hang)?.issue).toBe("10-22#8");
        expect(knownIssue({ ...job, part: "mlir" } as Job, "linux", hang)).toBeUndefined();
        expect(knownIssue(job, "linux", hang)).toBeUndefined();
    });

    test("a known issue naming what the failure says matches no other text", async () => {
        const abseil = await findJob("regression", "abseil/clang");
        const diff = (lines: string[]) => ({
            kind: "disagree",
            request: "highlights",
            detail: ["--- inspect", "+++ server", "@@ -1,2 +1,3 @@", ...lines].join("\n"),
            repro: "",
        });
        const more = diff([" highlights = [", '+    { at = "⟦a⟧", kind = "text" },', " ]"]);
        expect(knownIssue(abseil, "linux", more)?.issue).toBe("10-24#7");
        const fewer = diff([" highlights = [", '-    { at = "⟦a⟧", kind = "text" },', " ]"]);
        expect(knownIssue(abseil, "linux", fewer)).toBeUndefined();
    });

    test("known failures move apart, the issues not seen listed", () => {
        const results: Parameters<typeof sortKnown>[2] = {
            failures: [
                { kind: "disagree", request: "signatureHelp", detail: "d", repro: "r" },
                { kind: "crash", detail: "c", repro: "r" },
            ],
            known: [],
            unseen: [],
        };
        sortKnown(job, "linux", results);
        expect(results.failures.map((failure) => failure.kind)).toEqual(["crash"]);
        expect(results.known).toEqual([
            {
                kind: "disagree",
                request: "signatureHelp",
                detail: "d",
                repro: "r",
                issue: "10-22#6",
            },
        ]);
        // 10-22#7 is macOS's, 10-22#8 another part's: neither is this job's.
        expect(results.unseen).toEqual([]);
        const onMac: Parameters<typeof sortKnown>[2] = { failures: [], known: [], unseen: [] };
        sortKnown(job, "macos", onMac);
        expect(onMac.unseen.map((known) => known.issue)).toEqual(["10-22#6", "10-22#7"]);
    });
});

describe("prepare", () => {
    test("a checkout prepared by another configuration is refused, not reused", async () => {
        const upstream = scratch();
        const dir = scratch();
        const git = (...args: string[]) =>
            execFileSync("git", ["-c", "user.name=t", "-c", "user.email=t@t", ...args], {
                cwd: upstream,
                encoding: "utf8",
            }).trim();
        fs.writeFileSync(path.join(upstream, "a.c"), "int a;\n");
        git("init", "-q");
        git("add", ".");
        git("commit", "-q", "-m", "a");
        const write = (flag: string) => [
            "node",
            "-e",
            `require("fs").writeFileSync("db.json", ${JSON.stringify(flag)})`,
        ];
        const project = (flag: string): Project => ({
            name: "p",
            about: "",
            repository: upstream,
            commit: git("rev-parse", "HEAD"),
            configurations: { c: { about: "", prepare: [write(flag)], database: "db.json" } },
        });
        const target = await prepare(project("-DOLD"), "c", dir);
        expect(await prepare(project("-DOLD"), "c", dir)).toBe(target);
        await expect(prepare(project("-DNEW"), "c", dir)).rejects.toThrow(
            "prepared by another configuration",
        );
        expect(fs.readFileSync(path.join(target, "db.json"), "utf8")).toBe("-DOLD");
        fs.rmSync(upstream, { recursive: true, force: true });
        fs.rmSync(dir, { recursive: true, force: true });
    });

    test("custom commands lose their order-only inputs, TableGen's .td outputs listed", () => {
        const { ninja, tablegenFiles } = withoutOrderOnly(
            [
                "build mlir/AccCommon.td | ${cmake_ninja_workdir}mlir/AccCommon.td: CUSTOM_COMMAND $",
                "    bin/mlir-tblgen /src/ACC.td || acc_common_td_deps",
                "build mlir/Linalg.yamlgen.td mlir/Linalg.yamlgen.cpp.inc: CUSTOM_COMMAND bin/mlir-linalg-ods-yaml-gen /src/Linalg.yaml || yaml_deps",
                "build mlir/AccOps.h.inc: CUSTOM_COMMAND bin/mlir-tblgen /src/AccOps.td || acc_common_td",
                "build lib/a.o: CXX_COMPILER a.cpp || generated_headers",
            ].join("\n"),
        );
        expect(tablegenFiles).toEqual(["mlir/AccCommon.td"]);
        expect(ninja.split("\n")).toEqual([
            "build mlir/AccCommon.td | ${cmake_ninja_workdir}mlir/AccCommon.td: CUSTOM_COMMAND bin/mlir-tblgen /src/ACC.td",
            "build mlir/Linalg.yamlgen.td mlir/Linalg.yamlgen.cpp.inc: CUSTOM_COMMAND bin/mlir-linalg-ods-yaml-gen /src/Linalg.yaml",
            "build mlir/AccOps.h.inc: CUSTOM_COMMAND bin/mlir-tblgen /src/AccOps.td",
            "build lib/a.o: CXX_COMPILER a.cpp || generated_headers",
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

    test("cl checks the code by /Zs, its precompiled header read from its source, no .pdb", () => {
        expect(
            syntaxOnly({
                directory: "/b",
                file: "a.cpp",
                arguments: [
                    "cl.exe",
                    "/nologo",
                    "/Zc:__cplusplus",
                    "/Zi",
                    "/Fdvc140.pdb",
                    "/Yucmake_pch.hxx",
                    "/Fpa.pch",
                    "/FIcmake_pch.hxx",
                    "/Foa.obj",
                    "/c",
                    "--",
                    "a.cpp",
                ],
            }),
        ).toEqual([
            "cl.exe",
            "/nologo",
            "/Zc:__cplusplus",
            "/FIcmake_pch.hxx",
            "/Zs",
            "--",
            "a.cpp",
        ]);
    });

    test("nvcc checks the code by compiling it into the null device", () => {
        expect(
            syntaxOnly({
                directory: "/b",
                file: "a.cu",
                command: "nvcc -arch=sm_80 -c a.cu -o a.o",
            }),
        ).toEqual(["nvcc", "-arch=sm_80", "a.cu", "-c", "-o", os.devNull]);
    });

    test("a warning is an error as the command's last word on it says", () => {
        const entry = (command: string) => ({ directory: "/b", file: "a.c", command });
        expect(warningIsError(entry("cc -Wall -c a.c"), "unused-result")).toBe(false);
        expect(warningIsError(entry("cc -Werror -c a.c"), "unused-result")).toBe(true);
        expect(warningIsError(entry("cc -Werror -Wno-error -c a.c"), "unused-result")).toBe(false);
        expect(
            warningIsError(entry("cc -Werror -Wno-error=unused-result -c a.c"), "unused-result"),
        ).toBe(false);
        expect(warningIsError(entry("cc -Werror=unused-result -c a.c"), "unused-result")).toBe(
            true,
        );
        expect(warningIsError(entry("cc -Werror=vla -c a.c"), "unused-result")).toBe(false);
        expect(warningIsError(entry("cl /W4 /WX /c a.c"), "unused-result")).toBe(true);
        expect(warningIsError(entry("cl /WX /WX- /c a.c"), "unused-result")).toBe(false);
    });

    test("a part's headers are those its longest prefixes hold", () => {
        const config: Configuration = {
            about: "",
            prepare: [],
            database: "db.json",
            parts: { llvm: ["llvm/", "build/"], clang: ["clang/", "build/tools/clang/"] },
        };
        const root = "/w";
        expect(inPart(config, "clang", root, "build/tools/clang/a.inc")).toBe(true);
        expect(inPart(config, "llvm", root, "build/tools/clang/a.inc")).toBe(false);
        expect(inPart(config, "llvm", root, "build/include/b.inc")).toBe(true);
        expect(inPart(config, "llvm", root, "third/c.h")).toBe(false);
        expect(
            inPart({ about: "", prepare: [], database: "db.json" }, undefined, root, "x.h"),
        ).toBe(true);
    });

    test("a part takes the units under its longest prefixes", () => {
        const root = scratch();
        try {
            const entries = ["one/a.cpp", "two/b.cpp", "two/c.cpp", "one/two/d.cpp"].map(
                (file) => ({
                    directory: root,
                    file: path.join(root, file),
                    arguments: ["clang++", "-c", file],
                }),
            );
            fs.writeFileSync(path.join(root, "db.json"), JSON.stringify(entries));
            const config: Configuration = {
                about: "",
                prepare: [],
                database: "db.json",
                parts: { one: ["one/"], two: ["two/", "one/two/"] },
            };
            const own = jobDatabase(config, "two", root);
            expect(own.entries.map((entry) => path.relative(root, entry.file))).toEqual([
                "two/b.cpp",
                "two/c.cpp",
                "one/two/d.cpp",
            ]);
            expect(own.directory).toBe(path.join(root, ".ecosystem"));
            expect(fs.readFileSync(path.join(root, "clice.toml"), "utf8")).toBe(
                '[[rules]]\ncompile_commands = [".ecosystem/compile_commands.json"]\n',
            );
            expect(jobDatabase(config, undefined, root).entries).toHaveLength(4);
            const uncovered = { ...config, parts: { one: ["one/"] } };
            expect(() => jobDatabase(uncovered, "one", root)).toThrow(/two\/b.cpp is in no part/);
            const shared = { ...config, parts: { one: ["one/", "two/"], two: ["two/"] } };
            expect(() => jobDatabase(shared, "one", root)).toThrow(
                /two\/b.cpp is in the parts one, two/,
            );
        } finally {
            fs.rmSync(root, { recursive: true });
        }
    });

    test("a thinned database keeps units spread over it", () => {
        const entries = Array.from({ length: 10 }, (_, i) => ({
            directory: "/p",
            file: `/p/f${String(9 - i)}.cpp`,
            arguments: [],
        }));
        expect(thinned(entries, 3).map((entry) => entry.file)).toEqual([
            "/p/f1.cpp",
            "/p/f5.cpp",
            "/p/f8.cpp",
        ]);
        const many = Array.from({ length: 239 }, (_, i) => ({
            directory: "/p",
            file: `/p/f${String(i).padStart(3, "0")}.cpp`,
            arguments: [],
        }));
        const kept = thinned(many, 120).map((entry) => entry.file);
        expect(kept).toHaveLength(120);
        expect(kept[0]).toBe("/p/f000.cpp");
        expect(kept.at(-1)).toBe("/p/f238.cpp");
    });

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
});

describe("memory", () => {
    test("a process holds memory of its own", () => {
        expect(residentNow(process.pid).root).toBeGreaterThan(0);
    });

    test("a watch stopped before its first sample still has a peak", () => {
        const { peak, end, roles } = watchTree(process.pid, "runner").stop();
        expect(end.root).toBeGreaterThan(0);
        expect(peak.root).toBeGreaterThanOrEqual(end.root);
        expect(peak.total).toBeGreaterThanOrEqual(end.root + end.children);
        expect(roles["runner"]).toBe(end.root);
    });

    test("macOS's and Windows's listings read alike", () => {
        expect(
            parsed([
                "M\t10\t2048",
                "P\t10\t1\t1-02:03:04.50\t/bin/clice serve",
                "P\t11\t10\t0:01.25\t/bin/clice worker --worker-name SL-1",
            ]),
        ).toEqual([
            { pid: 10, parent: 1, own: 2, peak: 2, cpu: 93784.5, args: "/bin/clice serve" },
            {
                pid: 11,
                parent: 10,
                own: 0,
                peak: 0,
                cpu: 1.25,
                args: "/bin/clice worker --worker-name SL-1",
            },
        ]);
        expect(parsed(["W\t20\t4\t4096\t8192\t638\t25000000\tclice.exe serve\r"])).toEqual([
            {
                pid: 20,
                parent: 4,
                own: 4,
                peak: 8,
                cpu: 2.5,
                started: 638,
                args: "clice.exe serve",
            },
        ]);
    });
});

describe("report", () => {
    const base: Results = {
        job: "p",
        platform: "linux",
        about: "v1",
        commit: "c",
        clice: "k",
        seed: "s",
        compilerRejects: [],
        logErrors: [],
        failures: [],
        known: [],
        unseen: [],
        accepted: [],
    };

    test("failures with their reproduction, numbers beside the previous round's", () => {
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
            ...base,
            failures: [
                { kind: "compile", file: "a.cpp", detail: "a.cpp:1:1: boom", repro: "run it" },
            ],
            accepted: [{ file: "b.cpp", label: "completion m0", case: "c/d/01_e" }],
            index,
        };
        const text = regressionReport(results, { ...base, index: { ...index, seconds: 90 } });
        expect(text).toContain("# regression p on linux: 1 new failures");
        expect(text).toContain("| index | 2.0 min | 90 s |");
        expect(text).toContain("## Units clice reports errors on, which the compiler compiles (1)");
        expect(text).toContain("run it");
        expect(text).toContain(
            "## Differences the project accepts, each pinned by a case (1)\n\n- b.cpp, completion m0: tests/c/d/01_e",
        );
    });

    test("known failures apart from new ones, the issues not seen listed", () => {
        const text = regressionReport({
            ...base,
            known: [
                {
                    kind: "disagree",
                    file: "a.cpp",
                    request: "signatureHelp",
                    at: "f(⟦⟧)",
                    detail: "- res_arg\n+ res",
                    repro: "r",
                    issue: "10-22#6",
                },
            ],
            unseen: [{ issue: "10-22#7", why: "too big" }],
        });
        expect(text).toContain("# regression p on linux: green, 1 known");
        expect(text).toContain(
            "## Known failures (1)\n\n### 10-22#6: a.cpp, signatureHelp at `f(⟦⟧)`",
        );
        expect(text).toContain("## Known issues not seen this round (1)\n\n- 10-22#7: too big");
    });

    test("one cause at many places is shown once", () => {
        const crash = (file: string) => ({
            kind: "crash",
            file,
            request: "hover",
            at: "i⟦⟧nt x;",
            detail: "clice inspect crashed (SIGKILL)",
            repro: `run ${file}`,
        });
        const text = regressionReport({
            ...base,
            failures: [crash("a.c"), crash("b.c"), crash("c.c")],
        });
        expect(text).toContain("## Crashes (3)");
        expect(text).toContain(
            "### a.c, hover at `i⟦⟧nt x;` and 2 more\n\n- b.c, hover at `i⟦⟧nt x;`\n- c.c, hover at `i⟦⟧nt x;`",
        );
        expect(text.match(/crashed \(SIGKILL\)/g)).toHaveLength(1);
        expect(text).toContain("run a.c");
        expect(text).not.toContain("run b.c");
    });

    test("the slow requests are listed, the slowest first", () => {
        const text = regressionReport({
            ...base,
            platform: "windows",
            sweep: {
                files: 1,
                serverOnly: [],
                requests: 2,
                inspectSeconds: 80,
                serverSeconds: 300,
                slow: [
                    { file: "a.cpp", label: "hover p0", seconds: 75 },
                    { file: "a.cpp", label: "references p3", seconds: 300 },
                ],
                memory: {
                    start: { root: 300, children: 0 },
                    peak: { root: 900, child: 2100, total: 5000 },
                    end: { root: 700, children: 400 },
                },
            },
        });
        expect(text).toContain(
            "| server memory: master + workers, at start / once all closed | 300 + 0 / 700 + 400 MB |  |",
        );
        expect(text).toContain(
            "| server peak memory: master / worker / all | 900 / 2100 / 5000 MB |  |",
        );
        expect(text).toContain("# regression p on windows: green");
        expect(text).toContain(
            "## Server requests that took over a minute (2)\n\n- a.cpp, references p3: 300 s\n- a.cpp, hover p0: 75 s",
        );
    });

    test("what fails nothing is listed: units the compiler rejects too, error lines", () => {
        const text = regressionReport({
            ...base,
            compilerRejects: [{ unit: "/w/a.c", output: "a.c:1:1: error: x\nmore" }],
            logErrors: ["master.log: [error] no"],
        });
        expect(text).toContain(
            "## Units the compiler rejects too (1)\n\n- `/w/a.c`: a.c:1:1: error: x",
        );
        expect(text).toContain(
            "## Error lines in clice's logs (1)\n\n```\nmaster.log: [error] no\n```",
        );
    });

    test("a detail holding a code block keeps its fence", () => {
        const text = regressionReport({
            ...base,
            failures: [
                {
                    kind: "disagree",
                    file: "a.cpp",
                    request: "hover",
                    at: "p0",
                    detail: "-```cpp\n+```c\n",
                    repro: "run it",
                },
            ],
        });
        expect(text).toContain("````\n-```cpp\n+```c\n\n````");
    });
});
