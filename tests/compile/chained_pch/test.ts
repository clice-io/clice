import { expect } from "vitest";
import { answers, test, type Serve, type TestContext } from "@clice/tools/testing";

// Every case is a CMake target with a precompiled header (base.h and what
// it includes), and main.cpp opens with a preamble of its own (extra.h): an
// open main.cpp builds its preamble's PCH over the target's, a chain of
// two, and the background index loads the target's PCH. The chain answers
// what a parse loading no PCH answers: clice inspect, which never loads
// one, and the same database with -include-pch dropped. Markers named c_
// complete, s_ ask for signature help, the others ask where things are.

const DATABASE = "build/compile_commands.json";
const WRITER = "build/CMakeFiles/app.dir/cmake_pch.hxx.cxx";
const PCH = ".clice/cache/*/pch/*.pch";

interface Entry {
    directory: string;
    command: string;
    file: string;
    output: string;
}

/// The database with no unit reading the target's PCH: each parses the
/// text of the header set through the forced include. The writer stays a
/// unit of its own.
function withoutPch(t: TestContext): void {
    const entries = JSON.parse(t.disk.read(DATABASE)) as Entry[];
    const plain = entries.map((entry) => ({
        ...entry,
        command: entry.command.replace(/-Xclang -include-pch -Xclang \S+ /, ""),
    }));
    t.disk.write(DATABASE, JSON.stringify(plain, null, 2));
}

function located(name: string): boolean {
    return !name.startsWith("c_") && !name.startsWith("s_");
}

answers("features", async (a) => {
    for (const m of a.markers()) {
        if (m.name.startsWith("c_")) {
            await a.show(m.name, a.completion(m));
        } else if (m.name.startsWith("s_")) {
            await a.show(m.name, a.signatureHelp(m));
        } else {
            await a.show(m.name, { hover: a.hover(m), highlights: a.highlights(m) });
        }
    }
    await a.show("symbols", a.documentSymbols("main.cpp"));
    await a.show("tokens", a.semanticTokens("main.cpp"));
    await a.show("hints", a.inlayHints("main.cpp"));
    await a.show("links", a.documentLinks("main.cpp"));
});

/// What the server answers at each marker; the chain's are shown.
async function locations(
    t: TestContext,
    s: Serve,
    show: boolean,
): Promise<Record<string, unknown>> {
    const answered: Record<string, unknown> = {};
    for (const m of t.markers().filter((m) => located(m.name))) {
        const asked = {
            hover: s.hover(m),
            definition: s.definition(m),
            declaration: s.declaration(m),
            references: s.references(m),
        };
        if (show) {
            await t.show(m.name, asked);
        }
        answered[m.name] = {
            hover: await asked.hover,
            definition: await asked.definition,
            declaration: await asked.declaration,
            references: await asked.references,
        };
    }
    return answered;
}

for (const indexing of [false, true]) {
    test(indexing ? "indexed locations" : "locations", async (t) => {
        const config = { project: { enable_indexing: indexing } };
        // Indexed before main.cpp opens: an open file's own compile stands
        // in for its index run, which a later open would race.
        const start = async (): Promise<Serve> => {
            const s = await t.start({ config });
            if (indexing) {
                await s.indexed();
            }
            return s;
        };
        const s = await start();
        const files = [...new Set(t.markers().map((m) => m.path))];
        const chained: Record<string, unknown> = {};
        for (const file of files) {
            const diagnostics = s.diagnostics(file);
            await t.show(file, diagnostics);
            chained[file] = await diagnostics;
        }
        const built = (await s.counts()).files;
        expect({ base: built[WRITER]?.pch, top: built["main.cpp"]?.pch }).toEqual({
            base: 1,
            top: 1,
        });
        const answered = await locations(t, s, true);
        await s.stop();

        t.disk.rm(".clice");
        withoutPch(t);
        const plain = await start();
        for (const file of files) {
            expect(await plain.diagnostics(file), file).toEqual(chained[file]);
        }
        expect(await locations(t, plain, false)).toEqual(answered);
    });
}

test("index", async (t) => {
    const questions = t
        .markers()
        .filter((m) => located(m.name))
        .map((m) => `query references ${t.place(m)} --include-declaration`);
    await t.cli("index");
    expect(t.disk.list(PCH)).toHaveLength(1);
    const chained: unknown[] = [];
    for (const question of questions) {
        chained.push((await t.cli(question)).json);
    }
    t.disk.rm(".clice");
    withoutPch(t);
    await t.cli("index");
    expect(t.disk.list(PCH)).toHaveLength(0);
    for (const [i, question] of questions.entries()) {
        expect((await t.cli(question)).json, question).toEqual(chained[i]);
    }
});
