import { answers } from "@clice/tools/testing";

// Import completion lists the modules the server's module map knows, which
// clice inspect, compiling one file alone, has none of.
answers({ paths: { server: "import completion reads the server's module map" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edit"] });
    }
});
