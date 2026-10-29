import { answers } from "@clice/tools/testing";

// The file sits in src/, its database in build/: inspect finds the database
// the server finds.
answers(async (a) => {
    await a.show("value", a.hover(a.marker("value")));
});
