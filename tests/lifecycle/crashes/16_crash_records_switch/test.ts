import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Choosing the application's source as the header's context moves the open
// header into the application's project.
test("switch to another project", async (t) => {
    const s = await t.start({
        folders: ["app", "lib"],
        crash: { request: "hover", file: "lib/include/lib.h" },
    });
    const at = t.at("lib/include/lib.h", "int |lib_fn");
    s.open("lib/include/lib.h");
    await s.sync();
    await t.show("hover", s.hover(at));
    await t.show("hover while paused", s.hover(at));
    await t.show("crashes", await s.crashes());
    expect((await s.switchContext("lib/include/lib.h", "app/main.cpp")).success).toBe(true);
    await s.sync();
    // The application's project says at once what crashed in the library's.
    await t.show("pushes", s.pushes("lib/include/lib.h"));
    // The new context is a change, retried after the pause.
    await t.elapse(2_100, "the spacing between a crash and the retry a change earns");
    await t.show("hover after the switch", s.hover(at));
    await t.show("crashes after the switch", await s.crashes());
});
