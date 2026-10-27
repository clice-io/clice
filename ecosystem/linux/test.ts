import { perf, regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-24#5",
            why: "a trace header naming its includer's structs only in prototypes is judged self-contained, opens without its includer and reports each struct as not visible outside the prototype (an error under -Werror)",
            kind: "compile",
            detail: /will not be visible outside of this function/,
        },
        {
            issue: "10-27#5",
            why: "the server gives a format string in a macro's body one string token, inspect splits out its specifiers",
            kind: "disagree",
            request: "semanticTokens",
            // A specifier the server lacks on a macro's line, and every line
            // the two answers differ in a string or specifier piece of one.
            detail: /(?<![\s\S])(?=[\s\S]*^-\s+(?:\{ )?at = '(?:#\s*define [^'\n]*|[^'\n]*\\)',(?:\n-)?\s+type = "formatSpecifier")(?![\s\S]*^(?!--- |\+\+\+ )[-+](?!\s+(?:\{ at = '(?:#\s*define [^'\n]*|[^'\n]*\\)', type = "(?:string|formatSpecifier)" \},|at = '(?:#\s*define [^'\n]*|[^'\n]*\\)',|type = "(?:string|formatSpecifier)",|\{|\},)$).*$)/m,
        },
    ],
});

perf("startup", {}, async (p) => {
    await p.sample(["kernel/sched/core.c", "fs/ext4/inode.c", "include/linux/sched.h"], {
        wipe: "cache",
    });
    await p.duringIndex("kernel/sched/core.c", { minutes: 25 });
});
