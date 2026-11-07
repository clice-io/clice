import { describe, expect, test } from "vitest";
import type { Fold, Link, OutlineSymbol, Token } from "../render/answers.ts";
import type { Range } from "../render/position.ts";
import {
    foldDifferences,
    linkDifferences,
    outlineDifferences,
    tokenDifferences,
} from "./index_answers.ts";

const span = (line: number, begin: number, end: number): Range => ({
    start: { line, character: begin },
    end: { line, character: end },
});

const token = (character: number, type: string, modifiers: string[] = []): Token => ({
    line: 0,
    character,
    length: 3,
    type,
    modifiers,
});

describe("tokenDifferences", () => {
    test("a token or a modifier the index leaves out passes", () => {
        const ast = [token(0, "variable", ["declaration", "readonly"]), token(4, "function")];
        expect(tokenDifferences("a.cpp", ast, [token(0, "variable", ["declaration"])])).toEqual([]);
    });

    test("a type, length or modifier the AST does not give differs", () => {
        const ast = [token(0, "variable"), token(4, "function")];
        expect(
            tokenDifferences("a.cpp", ast, [
                token(0, "parameter"),
                { ...token(4, "function"), length: 5 },
                token(8, "macro"),
            ]),
        ).toHaveLength(3);
        expect(tokenDifferences("a.cpp", ast, [token(0, "variable", ["static"])])).toHaveLength(1);
    });

    test("a token inside code the AST dims passes", () => {
        const ast = [{ ...token(0, "comment", ["inactive"]), length: 20 }];
        expect(tokenDifferences("a.cpp", ast, [token(4, "keyword")])).toEqual([]);
    });
});

describe("foldDifferences", () => {
    const ast: Fold[] = [{ range: span(0, 10, 20), kind: "functionBody", collapsedText: "{...}" }];

    test("a fold without a kind passes", () => {
        expect(
            foldDifferences("a.cpp", ast, [{ range: span(0, 10, 20), collapsedText: "{...}" }]),
        ).toEqual([]);
    });

    test("a fold elsewhere or of another kind differs", () => {
        expect(
            foldDifferences("a.cpp", ast, [
                { range: span(0, 10, 21), collapsedText: "{...}" },
                { range: span(0, 10, 20), kind: "struct", collapsedText: "{...}" },
            ]),
        ).toHaveLength(2);
    });
});

describe("outlineDifferences", () => {
    const symbol = (
        name: string,
        line: number,
        children: OutlineSymbol[] = [],
        detail?: string,
    ): OutlineSymbol => ({
        name,
        kind: "Function",
        selection: { path: "a.cpp", range: span(line, 0, name.length) },
        detail,
        children,
    });

    test("an entry or a detail the index leaves out passes", () => {
        const ast = [symbol("outer", 0, [symbol("inner", 1, [], "int ()")]), symbol("free", 3)];
        expect(
            outlineDifferences("a.cpp", ast, [symbol("outer", 0, [symbol("inner", 1)])]),
        ).toEqual([]);
    });

    test("an entry only the index lists, and what it nests, differ", () => {
        const ast = [symbol("free", 3)];
        const index = [symbol("section", 0, [symbol("free", 3)])];
        expect(outlineDifferences("a.cpp", ast, index)).toEqual([
            "document symbols a.cpp:1:1: only the index lists Function section",
            "document symbols a.cpp:4:1: the parent is (top level) in the AST, section in the index",
        ]);
    });
});

describe("linkDifferences", () => {
    const link = (begin: number, target: string): Link => ({
        location: { path: "a.cpp", range: span(0, begin, begin + 5) },
        target,
    });

    test("a link the index leaves out passes; another target differs", () => {
        const ast = [link(10, "a.h"), link(30, "b.h")];
        expect(linkDifferences("a.cpp", ast, [link(10, "a.h")])).toEqual([]);
        expect(linkDifferences("a.cpp", ast, [link(10, "c.h"), link(50, "d.h")])).toHaveLength(2);
    });
});
