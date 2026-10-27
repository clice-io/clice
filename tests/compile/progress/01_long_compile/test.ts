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
    const token = `clice/compile/${s.uri("main.cpp")}`;
    await s.client.progressed(token, "begin");
    await park.release();
    await pulled;
    await s.client.progressed(token, "end");
    await t.show(
        "progress",
        s.client.progressEvents
            .filter((event) => event.token === token)
            .map((event) => event.value),
    );
});
