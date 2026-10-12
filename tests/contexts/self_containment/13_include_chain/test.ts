import { test } from "@clice/tools/testing";

// main.cpp enters bar.h through foo.h; its own include of bar.h then finds
// the guard. When foo.h moves its include, the recorded chain no longer
// holds and main.cpp is read again.
test(async (t) => {
    const s = await t.start();
    await t.show("bar.h", s.diagnostics("bar.h"));
    const written = t.disk.mtime("foo.h");
    t.disk.edit("foo.h", { replace: '#include "bar.h"', with: '\n#include "bar.h"' });
    t.disk.touch("foo.h", new Date(written.getTime() + 10_000));
    await t.show("moved", s.diagnostics("bar.h"));
});
