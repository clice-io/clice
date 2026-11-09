import { answers } from "@clice/tools/testing";

// Two files declare util; z/main.cpp imports z's, as near as any module
// can be, and shows its documentation, while beta lies a directory further
// up. main.cpp, as near to both, imports the first by path, a's.
answers({ paths: { server: "import completion reads the server's module map" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edit", "documentation"] });
    }
});
