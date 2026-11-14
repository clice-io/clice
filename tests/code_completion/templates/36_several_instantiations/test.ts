import { answers } from "@clice/tools/testing";

// A template the code instantiates more than once offers the names every
// instantiation has, their overloads each listed: a call before the
// completion point counts, a generic lambda's calls count, from where it is
// written or from the template it is passed to, a branch of an
// `if constexpr` counts the instantiations taking it, and past eight
// instantiations the first eight tell. One whose object is no class leaves
// nothing to share.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m));
    }
});
