import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A .clang-format with a misspelled key, which clang-format refuses: the
// editor's formatting fails with the reason, and so does the command. The
// one in marked/ is saved with a byte order mark, and the reason names the
// place on its first line where the editor shows it.

test("editor", async (t) => {
    const s = await t.start();
    for (const file of ["main.cpp", "marked/main.cpp"]) {
        s.open(file);
        const error = await s.formatting(file).then(
            () => null,
            (failure: unknown) => failure,
        );
        expect(error).toBeInstanceOf(proto.ResponseError);
        const { code, message } = error as proto.ResponseError<unknown>;
        await t.show(file, { code, message });
    }
});

test("command", async (t) => {
    await t.cli("format");
});
