import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { materialize, mergeProjects, readProject, writeDatabases } from "./project.ts";

describe("mergeProjects", () => {
    test("lists add up, the rest is replaced", () => {
        expect(
            mergeProjects([
                { flags: ["-std=c++20"], compiles: false },
                { flags: ["-DCASE"], units: { "main.cpp": [] }, compiles: true },
            ]),
        ).toEqual({ flags: ["-std=c++20", "-DCASE"], units: { "main.cpp": [] }, compiles: true });
    });
});

describe("materialize", () => {
    test("a copy strips markers, expands inputs and writes the database", () => {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-src-"));
        const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-copy-")));
        try {
            fs.writeFileSync(path.join(dir, "main.cpp"), "int §(x)value;\n");
            fs.writeFileSync(path.join(dir, "flags.rsp.in"), "-I${WS}/include\n");
            fs.writeFileSync(path.join(dir, "test.ts"), "");
            const files = materialize(dir, root);
            writeDatabases({ flags: ["-DX"] }, files, root);
            expect(files).toEqual(["flags.rsp", "main.cpp"]);
            expect(fs.readFileSync(path.join(root, "main.cpp"), "utf8")).toBe("int value;\n");
            expect(fs.readFileSync(path.join(root, "flags.rsp"), "utf8")).toBe(
                `-I${root}/include\n`,
            );
            const database = JSON.parse(
                fs.readFileSync(path.join(root, "compile_commands.json"), "utf8"),
            ) as {
                arguments: string[];
            }[];
            expect(database.map((entry) => entry.arguments.slice(0, 3))).toEqual([
                ["clang++", "-DX", "-fsyntax-only"],
            ]);
        } finally {
            fs.rmSync(dir, { recursive: true });
            fs.rmSync(root, { recursive: true });
        }
    });
});

describe("readProject", () => {
    test("comments, and databases listing units", () => {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-src-"));
        const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-copy-")));
        try {
            fs.writeFileSync(
                path.join(dir, "project.toml"),
                '# Why the units are split.\n[units]\n"a.cpp" = ["-DA"]\n"b.cpp" = []\n\n[databases]\nbuild = ["a.cpp"]\n',
            );
            const project = readProject(dir);
            expect(project.databases).toEqual({ build: ["a.cpp"] });
            fs.writeFileSync(path.join(dir, "a.cpp"), "");
            fs.writeFileSync(path.join(dir, "b.cpp"), "");
            writeDatabases(project, materialize(dir, root), root);
            expect(fs.existsSync(path.join(root, "compile_commands.json"))).toBe(false);
            const database = JSON.parse(
                fs.readFileSync(path.join(root, "build", "compile_commands.json"), "utf8"),
            ) as { file: string; arguments: string[] }[];
            expect(database.map((entry) => entry.arguments.slice(1, 2))).toEqual([["-DA"]]);
            expect(() => {
                writeDatabases({ ...project, databases: { ".": ["c.cpp"] } }, ["a.cpp"], root);
            }).toThrow(/lists c.cpp, which is not a unit/);
        } finally {
            fs.rmSync(dir, { recursive: true });
            fs.rmSync(root, { recursive: true });
        }
    });

    test("an error names the file and the key", () => {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-src-"));
        const file = path.join(dir, "project.toml");
        try {
            fs.writeFileSync(file, '[units]\n"a.cpp" = "-DA"\n');
            expect(() => readProject(dir)).toThrow(
                /project\.toml: units\."a\.cpp" is a list of strings/,
            );
            fs.writeFileSync(file, "flag = []\n");
            expect(() => readProject(dir)).toThrow(/project\.toml: unknown key 'flag'/);
            fs.writeFileSync(file, "flags = [\n");
            expect(() => readProject(dir)).toThrow(/project\.toml: Invalid TOML document/);
        } finally {
            fs.rmSync(dir, { recursive: true });
        }
    });
});
