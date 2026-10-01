/// Integration tests for the two folding modes a client can ask for. A
/// `lineFoldingOnly` client hides whole lines below the start line, so a
/// fold must end before the line holding its closing brace or the next
/// section's header, while a run of comment, include or using lines hides
/// its last line too.

import type * as proto from "vscode-languageserver-protocol";
import { expect, test } from "../fixtures.ts";

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
          typename U>
struct S {};
// a run ending
// the file
`;

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
`;

function render(folds: proto.FoldingRange[] | null): string[] {
    return (folds ?? []).map(
        (fold) =>
            `${fold.startLine}:${fold.startCharacter ?? "-"}-${fold.endLine}:${fold.endCharacter ?? "-"} ${fold.kind}`,
    );
}

for (const lineFoldingOnly of [true, false]) {
    test(`folds with lineFoldingOnly ${lineFoldingOnly}`, async ({ session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("main.cpp", MAIN);
        workspace.writeCDB(["main.cpp"]);
        await client.initialize(workspace, {
            capabilities: { textDocument: { foldingRange: { lineFoldingOnly } } },
        });
        const [uri] = await client.openAndWait("main.cpp");
        client.assertNoErrors(uri);

        const folds = render(await client.foldingRanges(uri));
        if (lineFoldingOnly) {
            expect(folds).toEqual([
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
            ]);
        } else {
            expect(folds).toEqual([
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
            ]);
        }
    });
}

for (const lineFoldingOnly of [true, false]) {
    test(`run folds with lineFoldingOnly ${lineFoldingOnly}`, async ({ session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("main.cpp", RUNS);
        for (const header of ["a.h", "b.h", "c.h"]) {
            workspace.write(header, "#pragma once\n");
        }
        workspace.writeCDB(["main.cpp"]);
        await client.initialize(workspace, {
            capabilities: { textDocument: { foldingRange: { lineFoldingOnly } } },
        });
        const [uri] = await client.openAndWait("main.cpp");
        client.assertNoErrors(uri);

        const folds = render(await client.foldingRanges(uri));
        if (lineFoldingOnly) {
            expect(folds).toEqual([
                "0:--1:- comment",
                "2:--4:- imports",
                "5:--6:- namespace",
                "8:--9:- usingDeclaration",
                "10:--11:- comment",
                "13:--14:- rawString",
                "19:--20:- comment",
            ]);
        } else {
            expect(folds).toEqual([
                "0:9-1:16 comment",
                "2:14-4:14 imports",
                "5:14-7:1 namespace",
                "8:13-9:13 usingDeclaration",
                "10:0-12:2 comment",
                "13:19-15:2 rawString",
                "16:9-17:21 templateParams",
                "19:15-20:11 comment",
            ]);
        }
    });
}

for (const lineFoldingOnly of [true, false]) {
    test(`declaration folds with lineFoldingOnly ${lineFoldingOnly}`, async ({ session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("main.cpp", DECLARATIONS);
        workspace.writeCDB(["main.cpp"]);
        await client.initialize(workspace, {
            capabilities: { textDocument: { foldingRange: { lineFoldingOnly } } },
        });
        const [uri] = await client.openAndWait("main.cpp");
        client.assertNoErrors(uri);

        const folds = render(await client.foldingRanges(uri));
        if (lineFoldingOnly) {
            expect(folds).toEqual([
                "1:--3:- functionBody",
                "6:--10:- functionBody",
                "6:--7:- functionParams",
                "12:--15:- class",
                "17:--19:- namespace",
                "21:--22:- conditionDirective",
                "23:--24:- conditionDirective",
                "26:--27:- functionBody",
                "29:--30:- functionBody",
            ]);
        } else {
            expect(folds).toEqual([
                "2:0-4:1 functionBody",
                "6:9-8:16 functionParams",
                "9:0-11:1 functionBody",
                "14:0-16:1 class",
                "18:0-20:1 namespace",
                "21:11-23:0 conditionDirective",
                "23:5-25:0 conditionDirective",
                "26:0-28:1 functionBody",
                "29:21-31:1 functionBody",
            ]);
        }
    });
}
