/// Integration tests for the two folding modes a client can ask for. A
/// `lineFoldingOnly` client hides whole lines below the start line, so a
/// fold must end before the line holding its closing brace or the next
/// section's header.

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
