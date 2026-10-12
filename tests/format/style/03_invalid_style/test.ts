import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A .clang-format with a misspelled key, which clang-format refuses: the
// editor's formatting fails with the reason, and so does the command.

test("editor", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    const error = await s.formatting("main.cpp").then(
        () => null,
        (failure: unknown) => failure,
    );
    expect(error).toBeInstanceOf(proto.ResponseError);
    const { code, message } = error as proto.ResponseError<unknown>;
    await t.show("main.cpp", { code, message });
});

test("command", async (t) => {
    await t.cli("format");
});
