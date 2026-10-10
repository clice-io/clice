/// Integration tests for the two folding modes a client can ask for. A
/// `lineFoldingOnly` client hides whole lines below the start line, so a
/// fold must end before the line holding its closing brace or the next
/// section's header, while a run of comment, include or using lines hides
/// its last line too, and a block whose brace sits below its declaration's
/// name starts on the name's line.

import type * as proto from "vscode-languageserver-protocol";
import { expect, serve } from "../../fixtures.ts";

const MAIN = `int f(int a) {
    if (a) {
        a += 1;
    } else {
        a -= 1;
    }
    try {
        a *= 2;
    } catch (...) {
        a = 0;
    }
    do {
        a -= 1;
    } while (a > 10);
    return a;
}
class Widget {
public:
    void draw();
private:
    int width;
};
#ifdef FLAG
int x;
#else
int y;
#endif
int g(int a,
      int b);
`;

const RUNS = `// run of
// line comments
#include "a.h"
#include "b.h"
#include "c.h"
namespace lib {
int a, b;
}
using lib::a;
using lib::b;
/* block
   comment
*/
const char* text = R"(
raw
)";
template <typename T,
          typename U,
          typename V>
struct S {};
// a run ending the file
// without a final newline`;

const DECLARATIONS = `struct Base {};
int process(int a)
{
    return a;
}
static int
gnu_style(int a,
          int b,
          int c)
{
    return a + b + c;
}
class Widget
    : public Base
{
    int x;
};
namespace
{
int y;
}
#ifdef FLAG
void picked(int a)
#else
void picked(int a, int b)
#endif
{
    return;
}
int same_line(int a) {
    return a;
}
void empty()
{
}
extern "C"
{
void c_function();
}
struct Holder
{
    int value;
    Holder()
        : value(0)
    {
        value += 1;
    }
};
`;

interface Case {
    name: string;
    source: string;
    headers?: string[];
    lines: string[];
    characters: string[];
}

const CASES: Case[] = [
    {
        name: "blocks and sections",
        source: MAIN,
        lines: [
            "0:--14:- functionBody",
            "1:--2:- compoundStmt",
            "3:--4:- compoundStmt",
            "6:--7:- compoundStmt",
            "8:--9:- compoundStmt",
            "11:--12:- compoundStmt",
            "16:--20:- class",
            "17:--18:- accessSpecifier",
            "19:--20:- accessSpecifier",
            "22:--23:- conditionDirective",
            "24:--25:- conditionDirective",
        ],
        characters: [
            "0:13-15:1 functionBody",
            "1:11-3:5 compoundStmt",
            "3:11-5:5 compoundStmt",
            "6:8-8:5 compoundStmt",
            "8:18-10:5 compoundStmt",
            "11:7-13:5 compoundStmt",
            "16:13-21:1 class",
            "17:7-19:0 accessSpecifier",
            "19:8-21:0 accessSpecifier",
            "22:11-24:0 conditionDirective",
            "24:5-26:0 conditionDirective",
            "27:5-28:12 functionParams",
        ],
    },
    {
        name: "runs and delimiters",
        source: RUNS,
        headers: ["a.h", "b.h", "c.h"],
        lines: [
            "0:--1:- comment",
            "2:--4:- imports",
            "5:--6:- namespace",
            "8:--9:- usingDeclaration",
            "10:--11:- comment",
            "13:--14:- rawString",
            "16:--17:- templateParams",
            "20:--21:- comment",
        ],
        characters: [
            "0:9-1:16 comment",
            "2:14-4:14 imports",
            "5:14-7:1 namespace",
            "8:13-9:13 usingDeclaration",
            "10:0-12:2 comment",
            "13:19-15:2 rawString",
            "16:9-18:21 templateParams",
            "20:24-21:26 comment",
        ],
    },
    {
        name: "declaration lines",
        source: DECLARATIONS,
        lines: [
            "1:--3:- functionBody",
            "6:--10:- functionBody",
            "6:--7:- functionParams",
            "12:--15:- class",
            "17:--19:- namespace",
            "21:--22:- conditionDirective",
            "23:--24:- conditionDirective",
            "26:--27:- functionBody",
            "29:--30:- functionBody",
            "35:--37:- linkageSpec",
            "39:--46:- struct",
            "42:--45:- functionBody",
        ],
        characters: [
            "2:0-4:1 functionBody",
            "6:9-8:16 functionParams",
            "9:0-11:1 functionBody",
            "14:0-16:1 class",
            "18:0-20:1 namespace",
            "21:11-23:0 conditionDirective",
            "23:5-25:0 conditionDirective",
            "26:0-28:1 functionBody",
            "29:21-31:1 functionBody",
            "33:0-34:1 functionBody",
            "36:0-38:1 linkageSpec",
            "40:0-47:1 struct",
            "44:4-46:5 functionBody",
        ],
    },
];

function render(folds: proto.FoldingRange[] | null): string[] {
    return (folds ?? []).map(
        (fold) =>
            `${fold.startLine}:${fold.startCharacter ?? "-"}-${fold.endLine}:${fold.endCharacter ?? "-"} ${fold.kind}`,
    );
}

for (const { name, source, headers = [], lines, characters } of CASES) {
    const files = {
        "main.cpp": source,
        ...Object.fromEntries(headers.map((header) => [header, "#pragma once\n"])),
    };
    const manifest = { cxx: ["-std=c++17"], units: { "main.cpp": [] } };

    serve.files(files, {
        manifest,
        launch: { capabilities: { textDocument: { foldingRange: { lineFoldingOnly: true } } } },
    })(`${name} with lineFoldingOnly true`, async ({ s }) => {
        expect(await s.errors("main.cpp")).toEqual([]);
        const folds = await s.request<proto.FoldingRange[] | null>(
            "textDocument/foldingRange",
            "main.cpp",
        );
        expect(render(folds)).toEqual(lines);
    });

    serve.files(files, { manifest })(`${name} with lineFoldingOnly false`, async ({ s }) => {
        expect(await s.errors("main.cpp")).toEqual([]);
        const folds = await s.request<proto.FoldingRange[] | null>(
            "textDocument/foldingRange",
            "main.cpp",
        );
        expect(render(folds)).toEqual(characters);
    });
}
