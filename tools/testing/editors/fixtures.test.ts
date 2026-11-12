import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, expect, test } from "vitest";
import { loadFixtures, prepareFixtures } from "./fixtures.ts";

const roots: string[] = [];

afterEach(() => {
    for (const root of roots.splice(0)) {
        fs.rmSync(root, { recursive: true, force: true });
    }
});

function temporary(prefix: string): string {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
    roots.push(root);
    return root;
}

test("each copy has its scenario, its databases and the test hooks on", () => {
    const target = path.join(temporary("clice-editors-"), "copies");
    const copies = prepareFixtures(target);
    expect(copies.map((copy) => path.basename(copy))).toEqual(
        loadFixtures().map((fixture) => fixture.name),
    );
    for (const copy of copies) {
        expect(fs.existsSync(path.join(copy, "scenario.json")), copy).toBe(true);
        expect(fs.existsSync(path.join(copy, "compile_commands.json")), copy).toBe(true);
        expect(fs.existsSync(path.join(copy, "project.toml")), copy).toBe(false);
        expect(fs.readFileSync(path.join(copy, "clice.toml"), "utf8")).toMatch(
            /^\[project\]\ntest_hooks = true\n$/m,
        );
    }
});

test("a fixture's own clice.toml keeps its rules", () => {
    const root = temporary("clice-editors-src-");
    const fixture = path.join(root, "01_rules");
    fs.mkdirSync(fixture);
    fs.writeFileSync(path.join(fixture, "main.cpp"), "int x;\n");
    fs.writeFileSync(path.join(fixture, "clice.toml"), '[[rules]]\nappend = ["-DX"]\n');
    fs.writeFileSync(
        path.join(fixture, "scenario.json"),
        JSON.stringify({
            file: "main.cpp",
            symbol: "x",
            indexed: "x",
            command: { source: "database" },
        }),
    );
    const copies = prepareFixtures(path.join(root, "copies"), root);
    expect(copies).toHaveLength(1);
    expect(fs.readFileSync(path.join(copies[0] ?? "", "clice.toml"), "utf8")).toBe(
        '[[rules]]\nappend = ["-DX"]\n\n[project]\ntest_hooks = true\n',
    );
});

test("a scenario naming what the fixture lacks fails", () => {
    const root = temporary("clice-editors-src-");
    const fixture = path.join(root, "01_missing");
    fs.mkdirSync(fixture);
    fs.writeFileSync(path.join(fixture, "main.cpp"), "int x;\n");
    fs.writeFileSync(
        path.join(fixture, "scenario.json"),
        JSON.stringify({
            file: "main.cpp",
            symbol: "y",
            indexed: "x",
            command: { source: "database" },
        }),
    );
    expect(() => loadFixtures(root)).toThrow('symbol: "y" is not in main.cpp');
});
