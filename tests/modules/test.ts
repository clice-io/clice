import { answers } from "@clice/tools/testing";

// The server opens every unit, interfaces first, and each must compile
// cleanly: an importer only does once the interfaces it imports are built.
// A marker sits on a use of an imported name, whose hover shows the import
// resolved.
answers(
    {
        paths: {
            server: "clice inspect compiles one file alone, without the interfaces it imports",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);
