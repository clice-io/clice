import { regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "11-15#26",
            why: "a name in a #define body that only the preamble expands gets no highlight from the server, which reads the preamble from its PCH",
            kind: "disagree",
            request: "highlights",
            file: "category_loader.c",
            detail: /^-\s+\{ at = "#define BUFFER_TYPE struct ⟦objc_category⟧ \*", kind = "text" \},$/m,
        },
    ],
});
