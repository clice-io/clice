import { answers } from "@clice/tools/testing";

// What the template resolver answers at each marker, as
// `clice inspect template-resolver` shows it: the type an alias or a type
// resolves to and the class it names, an `auto` variable's deduced type,
// the declarations a dependent name finds, what a call may call.
answers(
    { paths: { inspect: "the template resolver is clice's own, only clice inspect asks it" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.templateResolver(m));
        }
    },
);
