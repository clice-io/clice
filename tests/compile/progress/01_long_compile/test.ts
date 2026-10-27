import { test } from "@clice/tools/testing";

// An open document's compile that runs long, here a parse a park holds,
// shows a work-done progress naming what it waits on, which ends with the
// compile. The quick compile after an edit shows none: the progress is
// announced only once the compile has run a while.

test(async (t) => {
    const s = await t.start();
    await s.sync();
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("main.cpp", { pull: false });
    const pulled = s.diagnostics("main.cpp");
    await park.reached();
    // The token names the document by the server's spelling of its URI;
    // main.cpp is the only document compiling.
    const compile = (token: string) => token.startsWith("clice/compile/");
    await s.client.progressed(compile, "begin");
    await park.release();
    await pulled;
    await s.client.progressed(compile, "end");
    const events = s.client.progressEvents.filter((event) => compile(event.token));
    await t.show(
        "progress",
        events.map((event) => {
            const { kind, title, message } = event.value as {
                kind: string;
                title?: string;
                message?: string;
            };
            return [kind, title, message].filter((part) => part !== undefined).join(" · ");
        }),
    );
});
