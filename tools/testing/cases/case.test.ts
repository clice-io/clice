import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { loadCases, parseReadme, shard, type Case } from "./case.ts";

const page = (
    front: string,
    body = "# Use before definition\n\nA use above the define has no card\n",
) => `---\n${front}\n---\n\n${body}`;

describe("parseReadme", () => {
    test("a documented case", () => {
        const readme = parseReadme(page("issues: [clangd#2642]"), "x");
        expect(readme.title).toBe("Use before definition");
        expect(readme.summary).toBe("A use above the define has no card");
        expect(readme.issues).toEqual(["clangd#2642"]);
        expect(parseReadme("# Use before definition\n\nIt reads\n", "x").issues).toEqual([]);
    });

    test("malformed pages are refused", () => {
        expect(() => parseReadme(page("status: supported"), "x")).toThrow(/unknown key/);
        expect(() =>
            parseReadme(page("issues: []", "# One two three four five six\n\nSummary\n"), "x"),
        ).toThrow(/five words/);
        expect(() => parseReadme(page("issues: []", "# Title\n\nA summary.\n"), "x")).toThrow(
            /final period/,
        );
    });
});

describe("loadCases", () => {
    test("a script nobody runs", () => {
        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-cases-"));
        const write = (file: string, text: string): void => {
            fs.mkdirSync(path.dirname(path.join(root, file)), { recursive: true });
            fs.writeFileSync(path.join(root, file), text);
        };
        write("index/batch/01_one/main.cpp", "int main() {}\n");
        write("index/batch/01_one/test.ts", "");
        expect(loadCases(root)).toHaveLength(1);
        write("index/batch/test.ts", "");
        expect(() => loadCases(root)).toThrow(/index\/batch\/test.ts: no case runs this script/);
        fs.rmSync(root, { recursive: true });
    });

    test("a marker name has one place", () => {
        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-cases-"));
        const write = (file: string, text: string): void => {
            fs.mkdirSync(path.dirname(path.join(root, file)), { recursive: true });
            fs.writeFileSync(path.join(root, file), text);
        };
        write("hover/test.ts", "");
        write("hover/macros/01_one/main.cpp", "int §(x)a; int §(y)⟦b⟧;\n");
        expect(loadCases(root)).toHaveLength(1);
        write("hover/macros/01_one/main.cpp", "int §(x)⟦§(x)a⟧;\n");
        expect(() => loadCases(root)).toThrow(/also in main.cpp/);
        write("hover/macros/01_one/main.cpp", "int §(x)a;\n");
        write("hover/macros/01_one/other.cpp", "int §(x)b;\n");
        expect(() => loadCases(root)).toThrow(/also in main.cpp/);
        fs.rmSync(root, { recursive: true });
    });

    test("markers in source order", () => {
        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-cases-"));
        const write = (file: string, text: string): void => {
            fs.mkdirSync(path.dirname(path.join(root, file)), { recursive: true });
            fs.writeFileSync(path.join(root, file), text);
        };
        write("hover/test.ts", "");
        write("hover/macros/01_one/b.cpp", "int §b; int §(z)c;\n");
        write("hover/macros/01_one/a.cpp", "int §(y)⟦é⟧; int §a;\n");
        const [c] = loadCases(root);
        expect(
            c?.markers.map(({ path: file, name, begin, end, range }) => [
                file,
                name,
                begin,
                end,
                range,
            ]),
        ).toEqual([
            ["a.cpp", "y", 4, 5, true],
            ["a.cpp", "nameless_0", 11, 11, false],
            ["b.cpp", "nameless_1", 4, 4, false],
            ["b.cpp", "z", 11, 11, false],
        ]);
        fs.rmSync(root, { recursive: true });
    });
});

describe("shard", () => {
    const cases = ["a/x/01_p", "a/x/02_q", "a/x/03_r", "a/y/01_s", "b/z/01_t", "b/z/02_u"].map(
        (id) => {
            const [topic = "", subtopic = ""] = id.split("/");
            return { id, topic, subtopic } as Case;
        },
    );

    test("subtopics stay whole", () => {
        const shards = [1, 2].map((index) => shard(cases, `${String(index)}/2`).map((c) => c.id));
        expect(shards).toEqual([
            ["a/x/01_p", "a/x/02_q", "a/x/03_r"],
            ["a/y/01_s", "b/z/01_t", "b/z/02_u"],
        ]);
    });

    test("a malformed spec fails", () => {
        expect(() => shard(cases, "3/2")).toThrow(/not <index>\/<count>/);
        expect(() => shard(cases, "1")).toThrow(/not <index>\/<count>/);
    });
});
