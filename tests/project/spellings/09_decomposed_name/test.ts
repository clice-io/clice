import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const COMPOSED = "caf\u00e9.h";
const DECOMPOSED = "cafe\u0301.h";

test(
    "composed name of a decomposed file",
    {
        except: {
            linux: "the two forms name two different files",
            win32: "the two forms name two different files",
        },
    },
    async (t) => {
        // The header is on disk under its decomposed name, as a file made
        // on HFS+ keeps it; the source includes it, and the editor opens
        // it, by the composed one.
        t.disk.write(DECOMPOSED, "#pragma once\n\ninline int brew() {\n    return 1;\n}\n");
        const s = await t.start();
        expect(await s.errors("main.cpp")).toEqual([]);
        expect(await s.errors(COMPOSED)).toEqual([]);
        // One file: the source hosts the header the editor opened.
        await t.show("hosts", s.contexts(COMPOSED));
        // Results name it as the editor opened it.
        const call = t.at("main.cpp", "return |brew()");
        const targets = await s.request<{ uri: string }[]>("textDocument/definition", call);
        expect(targets.map(({ uri }) => s.spelling(uri))).toEqual([COMPOSED]);
    },
);
