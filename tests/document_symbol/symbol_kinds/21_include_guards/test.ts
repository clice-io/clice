import { answers } from "@clice/tools/testing";

// The headers, each opened in the context of the source that includes it.
// The markers only make the server open them.
answers(async (a) => {
    for (const file of ["widget.h", "defined.h", "unguarded.h"]) {
        await a.show(file, a.documentSymbols(file), { parts: ["detail"] });
    }
});
