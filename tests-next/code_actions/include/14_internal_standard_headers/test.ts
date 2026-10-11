import { answers } from "@clice/tools/testing";

// The index knows vector from libstdc++'s internal bits/stl_vector.h and
// deque from libc++'s __deque/deque.h: the actions offer the public headers
// alone. A name spelled with std:: asks the standard library mapping, never
// the index, which would offer MSVC's internal <xstring> too.

answers(
    {
        indexing: true,
        paths: { server: "unqualified names ask the project index, which only the server builds" },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.codeActions(m, { compile: true }));
        }
    },
);
