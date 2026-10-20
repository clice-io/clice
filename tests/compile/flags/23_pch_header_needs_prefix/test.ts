import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// main.cpp's command forces in cmake_pch.hxx, which includes config.h and
// then widget.h, which uses what config.h declares without including it;
// main.cpp includes widget.h alone. Without the forced header, which
// would enter it before its own text, widget.h does not compile alone: the
// server compiles it again after what cmake_pch.hxx holds before it, the
// context it has in main.cpp's compile. With main.cpp indexed, its tree,
// taken over the PCH, shows that only as main.cpp's include skipped;
// without the index, the tree of a run without the PCH shows it entered.

async function context(t: TestContext, indexed: boolean): Promise<void> {
    const s = await t.start({ config: { project: { enable_indexing: indexed } } });
    if (indexed) {
        await s.indexed();
    }
    await t.show("widget.h", s.diagnostics("widget.h"));
    for (const m of t.markers()) {
        await t.show(m.name, s.hover(m));
    }
    expect((await s.stats()).synthesizedContexts).toBe(1);
}

test("the context of an indexed host", (t) => context(t, true));

test("the context before the index", (t) => context(t, false));
