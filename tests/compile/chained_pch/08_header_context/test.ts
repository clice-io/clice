import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// widget.h is no file of its own: it opens in the context of main.cpp,
// whose command loads the target's PCH. Its preamble's PCH, built over the
// target's, holds main.cpp's text before the include; its trial without
// that text, which fails to stand alone, builds one too. The chain answers
// what the same database with -include-pch dropped answers.

const DATABASE = "build/compile_commands.json";
const WRITER = "build/CMakeFiles/app.dir/cmake_pch.hxx.cxx";
const config = { project: { enable_indexing: false } };

test("header context", async (t) => {
    const s = await t.start({ config });
    const chained = s.diagnostics("widget.h");
    await t.show("widget.h", chained);
    const { files } = await s.counts();
    expect({ base: files[WRITER]?.pch, header: files["widget.h"]?.pch }).toEqual({
        base: 1,
        header: 2,
    });
    const answered: Record<string, unknown> = {};
    for (const m of t.markers()) {
        const asked = { hover: s.hover(m), definition: s.definition(m) };
        await t.show(m.name, asked);
        answered[m.name] = { hover: await asked.hover, definition: await asked.definition };
    }
    await s.stop();

    t.disk.write(
        DATABASE,
        t.disk.read(DATABASE).replaceAll(/-Xclang -include-pch -Xclang \S+ /g, ""),
    );
    const plain = await t.start({ config });
    expect(await plain.diagnostics("widget.h")).toEqual(await chained);
    for (const [name, answer] of Object.entries(answered)) {
        const m = t.marker(name);
        expect(
            { hover: await plain.hover(m), definition: await plain.definition(m) },
            name,
        ).toEqual(answer);
    }
});
