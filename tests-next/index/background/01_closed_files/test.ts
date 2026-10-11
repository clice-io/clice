import { test } from "@clice/tools/testing";

// The usual session after the background index has run: main.cpp open, and
// nav.cpp and nav.h known only through the index. Every marker is asked
// every navigation request; one that answers nothing is left out of its
// block, except the definition, so no block comes out empty.
const OPEN = ["animal", "speak", "dog", "add", "compute", "add_call", "global_var"];
const CLOSED = [
    "blank",
    "area",
    "area_decl",
    "shape_type",
    "global_shape",
    "area_scale",
    "render_pure",
    "render_middle",
    "make_unit_shape",
];

test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    for (const name of [...OPEN, ...CLOSED]) {
        const m = t.marker(name);
        const asked = {
            definition: s.definition(m),
            declaration: s.declaration(m),
            type_definition: s.typeDefinition(m),
            implementation: s.implementation(m),
            references: s.references(m),
            uses: s.references(m, { declarations: false }),
            call_hierarchy: s.callHierarchy(m),
            incoming_calls: s.incomingCalls(m),
            outgoing_calls: s.outgoingCalls(m),
            type_hierarchy: s.typeHierarchy(m),
            supertypes: s.supertypes(m),
            subtypes: s.subtypes(m),
        };
        const answered = await Promise.all(Object.values(asked));
        await t.show(
            name,
            Object.fromEntries(
                Object.entries(asked).filter(
                    ([label], i) => label === "definition" || answered[i]?.length !== 0,
                ),
            ),
        );
    }
    await t.show("symbols", {
        add: s.workspaceSymbols("add"),
        animal: s.workspaceSymbols("Animal"),
    });
});
