import { test } from "@clice/tools/testing";

// Opened through a link, the file is offered the commands of the file it
// names, as the rules matching that file edit them.
test(async (t) => {
    t.disk.symlink("link", "real");
    const s = await t.start();
    s.open("link/main.cpp");
    await t.show("link/main.cpp", s.contexts("link/main.cpp"));
});
