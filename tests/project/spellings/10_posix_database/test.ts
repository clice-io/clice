import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const EXCEPT = {
    linux: "a POSIX path is native there",
    darwin: "a POSIX path is native there",
};

const UNMAPPED = "spells its paths POSIX-style";

// Git for Windows, which every Windows runner image ships, carries an MSYS2
// runtime: it spells C:\x as /c/x.
const GIT_RUNTIME = path.join(
    process.env["ProgramFiles"] ?? "C:\\Program Files",
    "Git",
    "usr",
    "bin",
);

const PATH = Object.keys(process.env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";

/// The runner's PATH without the runtime of any Cygwin or MSYS2.
function withoutRuntimes(): string[] {
    return (process.env[PATH] ?? "")
        .split(path.delimiter)
        .filter(
            (dir) =>
                !fs.existsSync(path.join(dir, "msys-2.0.dll")) &&
                !fs.existsSync(path.join(dir, "cygwin1.dll")),
        );
}

function posix(file: string): string {
    return `/${file.slice(0, 1).toLowerCase()}${file.slice(2).replaceAll("\\", "/")}`;
}

/// The database a build run inside MSYS2 writes for the case at `root`: its
/// compiler too is a POSIX path, the clang++ on PATH named without `.exe`.
function database(root: string): string {
    const clang = withoutRuntimes().find((dir) => fs.existsSync(path.join(dir, "clang++.exe")));
    if (clang === undefined) {
        throw new Error("no clang++.exe on PATH");
    }
    const entry = {
        directory: posix(root),
        file: posix(path.join(root, "main.cpp")),
        arguments: [
            posix(path.join(clang, "clang++")),
            `-I${posix(path.join(root, "inc dir"))}`,
            "-include",
            posix(path.join(root, "inc dir", "forced.h")),
            "-c",
            posix(path.join(root, "main.cpp")),
        ],
    };
    return JSON.stringify([entry], null, 2);
}

// The runtime on PATH maps the database's paths: the file has its entry,
// the entry its compiler, its include directory and its forced include.
test("mapped by the runtime", { except: EXCEPT }, async (t) => {
    t.disk.write("compile_commands.json", database(t.root));
    const s = await t.start({
        env: { [PATH]: [GIT_RUNTIME, ...withoutRuntimes()].join(path.delimiter) },
    });
    await t.show("main.cpp", s.errors("main.cpp"));
});

// Without one, the entries are skipped and the error says why.
test("no runtime on PATH", { except: EXCEPT, errors: [UNMAPPED] }, async (t) => {
    t.disk.write("compile_commands.json", database(t.root));
    const s = await t.start({ env: { [PATH]: withoutRuntimes().join(path.delimiter) } });
    await t.show("main.cpp", s.errors("main.cpp"));
    const logs = t.disk.list(".clice/logs/**/*.log").map((file) => t.disk.read(file));
    expect(logs.join("\n")).toContain(UNMAPPED);
});
