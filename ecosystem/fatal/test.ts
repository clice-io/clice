import { regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-24#7",
            why: "in templates and macro arguments the open file's highlights hold more than the unit's own",
            kind: "disagree",
            request: "highlights",
            // Every line the server adds, and inspect's empty answer.
            detail: /^(?:\+.*|-highlights = \[\])$/m,
        },
        {
            issue: "11-14#4",
            why: "the server's hover prints an alias's canonical type with a template parameter unnamed",
            kind: "disagree",
            request: "hover",
            file: "fatal/type/test/sanity_check_test.cpp",
            // The canonical type's line and the block the server prints for it.
            detail: /^[-+].*$/m,
        },
    ],
});
