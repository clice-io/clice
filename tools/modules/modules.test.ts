import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, describe, expect, test } from "vitest";
import {
    buildArguments,
    compdbFragments,
    compileCommands,
    compileFailures,
    CONFIGURATIONS,
    type Configuration,
    mergeFile,
    partition,
    stdModuleSources,
} from "./modules.ts";

const scratches: string[] = [];

function scratch(): string {
    const directory = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-modules-")));
    scratches.push(directory);
    return directory;
}

afterEach(() => {
    for (const directory of scratches.splice(0)) {
        fs.rmSync(directory, { recursive: true, force: true });
    }
});

function touch(file: string): void {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, "");
}

function configuration(name: string): Configuration {
    const found = CONFIGURATIONS.find((each) => each.name === name);
    if (found === undefined) {
        throw new Error(`no configuration ${name}`);
    }
    return found;
}

describe("configurations", () => {
    test("each holds a condition of its own, NDEBUG's by its build type", () => {
        expect(new Set(CONFIGURATIONS.map(({ name }) => name)).size).toBe(CONFIGURATIONS.length);
        expect(new Set(CONFIGURATIONS.map(({ condition }) => condition)).size).toBe(
            CONFIGURATIONS.length,
        );
        for (const { name, buildType, condition } of CONFIGURATIONS) {
            expect(
                condition.endsWith(buildType === "Debug" ? "!defined(NDEBUG)" : " defined(NDEBUG)"),
                name,
            ).toBe(true);
        }
    });

    test("the host's builds without a platform, the others cross-compile", () => {
        expect(buildArguments(configuration("linux-x64"), "/work/events.json")).toEqual([
            "build",
            "--config=RelWithDebInfo",
            "--aspects=@compdb//:compdb.bzl%compdb",
            "--output_groups=compdb,compilation_outputs",
            "--build_event_json_file=/work/events.json",
            "//modules:demand",
        ]);
        expect(buildArguments(configuration("macos-arm64-debug"), "/work/events.json")).toEqual([
            "build",
            "--config=Debug",
            "--platforms=@xclang//platforms:aarch64-apple-darwin",
            "--aspects=@compdb//:compdb.bzl%compdb",
            "--output_groups=compdb,compilation_outputs",
            "--build_event_json_file=/work/events.json",
            "//modules:demand",
        ]);
    });
});

describe("generate", () => {
    test("demand.cpp's fragment is the one compdb fragment the events name, once each", () => {
        const uri = "file:///out/k8%2Bopt/bin/modules/demand.compdb.json";
        const events = [
            `{"namedSetOfFiles":{"files":[{"name":"modules/demand.compdb.json","uri":"${uri}"}]}}`,
            `{"completed":{"importantOutput":[{"uri":"${uri}"}]}}`,
            `{"namedSetOfFiles":{"files":[{"uri":"file:///out/bin/src/clice.compdb.json"}]}}`,
        ].join("\n");
        expect(compdbFragments(events)).toEqual(["/out/k8%2Bopt/bin/modules/demand.compdb.json"]);
        expect(compdbFragments("")).toEqual([]);
    });

    test("the execution root replaces every placeholder, escaped for JSON and taken literally", () => {
        const fragment =
            '[{"directory":"__COMPDB_EXECROOT__","file":"__COMPDB_EXECROOT__/modules/demand.cpp"}]';
        expect(compileCommands(fragment, "C:\\b$&ase")).toBe(
            '[{"directory":"C:\\\\b$&ase","file":"C:\\\\b$&ase/modules/demand.cpp"}]',
        );
    });

    test("a library behind a symbolic link is globbed at both paths", () => {
        const outputBase = scratch();
        const contents = path.join(outputBase, "contents");
        for (const name of ["simdjson+", "kotatsu+", "croaring+", "tomlplusplus+", "lmdb+"]) {
            fs.mkdirSync(path.join(contents, name), { recursive: true });
        }
        fs.mkdirSync(path.join(outputBase, "external"));
        for (const name of ["simdjson+", "kotatsu+", "croaring+", "tomlplusplus+"]) {
            fs.symlinkSync(path.join(contents, name), path.join(outputBase, "external", name));
        }
        fs.mkdirSync(path.join(outputBase, "external", "lmdb+"));

        const external = `${outputBase}/external`;
        const xclang = `${external}/xclang++xclang+xclang_*`;
        const libclang = `${external}/xclang++xclang+libclang_*`;
        expect(partition(outputBase, "/opt/clice/bin/clice")).toEqual({
            modules: [
                { name: "libc", files: [`${xclang}/**/include/c++/v1/*.h`] },
                {
                    name: "std",
                    files: [`${xclang}/**/include/c++/v1/**`, `${xclang}/**/include/*/c++/v1/**`],
                    external: true,
                },
                {
                    name: "libc",
                    files: [`${xclang}/**`, `${libclang}/lib/clang/**`, "/opt/clice/lib/clang/**"],
                    textual: true,
                    provides: "std.compat",
                },
                {
                    name: "third_party",
                    files: [
                        `${libclang}/include/llvm/**`,
                        `${libclang}/include/llvm-c/**`,
                        `${libclang}/include/clang/**`,
                        `${libclang}/include/clang-c/**`,
                        `${libclang}/include/clang-tidy/**`,
                        `${contents}/simdjson+/**`,
                        `${external}/simdjson+/**`,
                        `${contents}/kotatsu+/**`,
                        `${external}/kotatsu+/**`,
                        `${contents}/croaring+/**`,
                        `${external}/croaring+/**`,
                        `${contents}/tomlplusplus+/**`,
                        `${external}/tomlplusplus+/**`,
                        `${external}/lmdb+/**`,
                    ],
                },
            ],
        });
    });

    test("libc++'s module sources are the target's, found through links and past hidden directories", () => {
        const outputBase = scratch();
        const toolchain = path.join(outputBase, "contents", "toolchain");
        const sources = "share/libc++/v1/std.cppm";
        touch(path.join(toolchain, "x86_64-unknown-linux-gnu", "usr", sources));
        touch(path.join(toolchain, "aarch64-apple-darwin", sources));
        touch(path.join(outputBase, "contents", "darwin-sdk", sources));
        fs.symlinkSync(
            path.join(outputBase, "contents", "darwin-sdk"),
            path.join(toolchain, "x86_64-apple-darwin"),
        );
        touch(path.join(toolchain, ".cache", "aarch64-w64-mingw32", sources));
        fs.mkdirSync(path.join(outputBase, "external"));
        fs.symlinkSync(
            toolchain,
            path.join(outputBase, "external", "xclang++xclang+xclang_x86_64"),
        );
        touch(
            path.join(
                outputBase,
                "external",
                "xclang++xclang+libclang_x86_64",
                "aarch64-w64-mingw32",
                sources,
            ),
        );

        const repository = path.join(outputBase, "external", "xclang++xclang+xclang_x86_64");
        expect(stdModuleSources(outputBase, "x86_64-unknown-linux-gnu")).toEqual([
            path.join(repository, "x86_64-unknown-linux-gnu", "usr", "share", "libc++", "v1"),
        ]);
        expect(stdModuleSources(outputBase, "x86_64-apple-darwin")).toEqual([
            path.join(repository, "x86_64-apple-darwin", "share", "libc++", "v1"),
        ]);
        expect(stdModuleSources(outputBase, "aarch64-w64-mingw32")).toEqual([]);
    });

    test("a unit that compiled with errors is reported by its first", () => {
        expect(
            compileFailures("linux-x64", {
                compiledWithErrors: [
                    {
                        unit: "/work/demand.cpp",
                        errors: [
                            {
                                file: "/work/demand.cpp",
                                line: 3,
                                message: "'gone.h' file not found",
                            },
                            { file: "/work/demand.cpp", line: 9, message: "unknown type name" },
                        ],
                    },
                    {
                        unit: "/work/other.cpp",
                        errors: [{ file: "/work/a.h", line: 1, message: "expected ';'" }],
                    },
                ],
            }),
        ).toEqual([
            "linux-x64: /work/demand.cpp does not compile: /work/demand.cpp:3: 'gone.h' file not found",
            "linux-x64: /work/other.cpp does not compile: /work/a.h:1: expected ';'",
        ]);
        expect(compileFailures("linux-x64", { compiledWithErrors: [] })).toEqual([]);
    });
});

describe("merge", () => {
    test("the merge file lists each configuration with its condition and its directory, in order", () => {
        const configurations = [
            configuration("linux-x64"),
            configuration("macos-arm64"),
            configuration("windows-x64"),
        ];
        expect(mergeFile(configurations)).toBe(`{
  "configurations": [
    {
      "name": "linux-x64",
      "condition": "defined(__linux__) && defined(__x86_64__) && __has_include(<gnu/libc-version.h>) && defined(NDEBUG)",
      "out": "linux-x64"
    },
    {
      "name": "macos-arm64",
      "condition": "defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)",
      "out": "macos-arm64"
    },
    {
      "name": "windows-x64",
      "condition": "defined(_WIN32) && defined(__x86_64__) && defined(NDEBUG)",
      "out": "windows-x64"
    }
  ]
}`);
    });
});
