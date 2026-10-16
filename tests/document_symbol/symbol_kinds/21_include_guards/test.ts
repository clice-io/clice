import { answers } from "@clice/tools/testing";

// The headers, each opened in the context of the source that includes it;
// the markers only make the server open them. widget.h's guard is how it
// keeps itself from being read twice, nothing it declares; unguarded.h
// defines its macro under a condition that leaves code outside it, and
// limits.h gives a macro a default unless the includer set it: neither
// guards anything. needs_host.h compiles only after what its includer
// declares before it.
answers(async (a) => {
    for (const file of ["widget.h", "defined.h", "unguarded.h", "limits.h", "needs_host.h"]) {
        await a.show(file, a.documentSymbols(file), { parts: ["detail"] });
    }
});
