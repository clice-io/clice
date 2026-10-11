/// Every rendering resolves to exactly the range it came from, over every
/// source of every case: identifiers, the points before them, brace blocks.

import * as fs from "node:fs";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import {
    type Location,
    type Range,
    type RenderScope,
    SourceText,
    renderLocation,
    resolveLocation,
} from "./position.ts";
import { parseAnnotations } from "../../snap/annotation.ts";
import { loadCases } from "../cases/case.ts";
import { CASES_DIR, MARKED } from "../cases/project.ts";

function sources(): Map<string, SourceText> {
    const out = new Map<string, SourceText>();
    for (const c of loadCases(CASES_DIR)) {
        for (const rel of c.inputs.filter((input) => MARKED.test(input))) {
            const text = fs.readFileSync(path.join(c.dir, rel), "utf8");
            out.set(`${c.id}/${rel}`, new SourceText(rel, parseAnnotations(text).content));
        }
    }
    return out;
}

function identifiers(source: SourceText): Range[] {
    const out: Range[] = [];
    source.lines.forEach((line, index) => {
        for (const match of line.matchAll(/[A-Za-z_]\w*/g)) {
            const start = { line: index, character: match.index };
            out.push({ start, end: { line: index, character: match.index + match[0].length } });
            out.push({ start, end: start });
        }
    });
    return out;
}

function blocks(source: SourceText): Range[] {
    const out: Range[] = [];
    const open: number[] = [];
    for (let offset = 0; offset < source.text.length; offset++) {
        if (source.text[offset] === "{") {
            open.push(offset);
        } else if (source.text[offset] === "}") {
            const start = open.pop();
            if (start !== undefined) {
                out.push({ start: source.positionAt(start), end: source.positionAt(offset + 1) });
            }
        }
    }
    return out;
}

const files = sources();
const scopeFor = (query: RenderScope["query"]): RenderScope => ({
    file: (file) => {
        const source = files.get(file);
        if (source === undefined) {
            throw new Error(`no file ${file}`);
        }
        return source;
    },
    query,
});

describe("round trip", () => {
    for (const [file, source] of files) {
        test(file, () => {
            const ranges = [...identifiers(source), ...blocks(source)];
            const scopes = [
                scopeFor({ path: file, line: 0 }),
                scopeFor({ path: file, line: source.lines.length - 1 }),
                scopeFor({ path: "elsewhere.cpp", line: 0 }),
                scopeFor(undefined),
            ];
            const failures: string[] = [];
            for (const range of ranges) {
                const location: Location = { path: file, range };
                for (const scope of scopes) {
                    const rendered = renderLocation(location, scope);
                    const back = resolveLocation(rendered, scope);
                    if (JSON.stringify(back) !== JSON.stringify(location)) {
                        failures.push(`${rendered} -> ${JSON.stringify(back.range)}`);
                    }
                }
            }
            expect(failures).toEqual([]);
        });
    }
});

describe("positions", () => {
    const text = 'int 名字 = 1;\r\nauto s = "😀"; int x;\n';
    const source = new SourceText("u.cpp", text);

    test("UTF-16 columns", () => {
        expect(source.lines).toEqual(["int 名字 = 1;", 'auto s = "😀"; int x;', ""]);
        expect(source.positionAt(text.indexOf("x"))).toEqual({ line: 1, character: 19 });
        expect(source.offsetAt({ line: 1, character: 19 })).toBe(text.indexOf("x"));
    });

    test("bytes from inspect", () => {
        const bytes = Buffer.byteLength(text.slice(0, text.indexOf("x")));
        expect(source.positionAtByte(bytes)).toEqual({ line: 1, character: 19 });
    });

    test("non-ASCII round trip", () => {
        const scope: RenderScope = { file: () => source, query: { path: "u.cpp", line: 0 } };
        const name = {
            path: "u.cpp",
            range: { start: { line: 0, character: 4 }, end: { line: 0, character: 6 } },
        };
        expect(renderLocation(name, scope)).toBe("int ⟦名字⟧ = 1;");
        expect(resolveLocation("int ⟦名字⟧ = 1;", scope)).toEqual(name);
    });

    test("repeated line gets an offset", () => {
        const twice = new SourceText("t.cpp", "void f();\nint a;\nvoid f();\n");
        const scope: RenderScope = { file: () => twice, query: { path: "t.cpp", line: 1 } };
        const second = {
            path: "t.cpp",
            range: { start: { line: 2, character: 5 }, end: { line: 2, character: 6 } },
        };
        expect(renderLocation(second, scope)).toBe("void ⟦f⟧(); @+1");
        expect(renderLocation(second, { ...scope, query: { path: "o.cpp", line: 0 } })).toBe(
            "t.cpp │ void ⟦f⟧(); @3",
        );
    });

    test("ambiguous text is refused", () => {
        const twice = new SourceText("t.cpp", "void f();\nvoid f();\n");
        expect(() =>
            resolveLocation("void ⟦f⟧();", {
                file: () => twice,
                query: { path: "t.cpp", line: 0 },
            }),
        ).toThrow(/names no unique range/);
    });
});
