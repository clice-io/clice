import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// One file, one buffer: a document naming an open file through a symlink
// shares the first document's answers while their texts agree, gets none
// once they diverge, never edits the first document's buffer, and takes
// over when the first closes.

const POSIX = { except: { win32: "symlinks need privileges on Windows" } };

async function refusal(reply: Promise<unknown>): Promise<{ code: number; message: string }> {
    const error = await reply.then(
        () => null,
        (failure: unknown) => failure,
    );
    expect(error).toBeInstanceOf(proto.ResponseError);
    const { code, message } = error as proto.ResponseError<unknown>;
    return { code, message };
}

test("a second name", POSIX, async (t) => {
    t.disk.write("real/main.cpp", t.disk.read("main.cpp"));
    t.disk.rm("main.cpp");
    const s = await t.start();
    t.disk.symlink("link", "real");
    await t.show("first", s.diagnostics("real/main.cpp"));
    await t.show("second", s.diagnostics("link/main.cpp"));
    await t.show("second hover", s.hover(t.at("link/main.cpp", "int |main")));
    s.edit("link/main.cpp", { replace: "return 0", with: "return undefined_name" });
    await t.show("second diverged", s.diagnostics("link/main.cpp"));
    await t.show(
        "second hover diverged",
        await refusal(s.hover(t.at("link/main.cpp", "int |main"))),
    );
    await t.show("second references", s.references(t.at("link/main.cpp", "int |main")));
    s.close("link/main.cpp");
    await t.show("second pushes", s.pushes("link/main.cpp"));
    await t.show("first hover", s.hover(t.at("real/main.cpp", "int |main")));
    await t.show("first after", s.diagnostics("real/main.cpp"));
    s.close("real/main.cpp");
    await s.sync();
    await t.show("sessions", (await s.stats()).sessions);
});

test("the second takes over", POSIX, async (t) => {
    t.disk.write("real/main.cpp", t.disk.read("main.cpp"));
    t.disk.rm("main.cpp");
    const s = await t.start();
    t.disk.symlink("link", "real");
    await s.diagnostics("real/main.cpp");
    s.open("link/main.cpp");
    s.edit("link/main.cpp", { replace: "return 0", with: "return undefined_name" });
    s.close("real/main.cpp");
    await t.show("second", s.diagnostics("link/main.cpp"));
});
