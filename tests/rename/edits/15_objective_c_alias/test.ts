import { answers } from "@clice/tools/testing";

// An alias names the class: a rename changes it where the alias declares
// it, and leaves the alias's own name as it is. A category of one class
// renames apart from another class's of the same name.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("class", a.rename(a.marker("pen"), "Brush"));
    await a.show("category", a.rename(a.marker("pen_extras"), "Motion"));
    await a.show("alias", a.rename(a.marker("quill"), "Nib"));
});
