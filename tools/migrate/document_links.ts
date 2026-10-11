/// One-off check of the document_links migration: every link an old
/// snapshot pinned is in the case it moved to, rendered the way the new
/// output names it, its target relative to the case.
///
///     node tools/migrate/document_links.ts

import {
    asked,
    bare,
    corpus,
    newBlocks,
    oldBody,
    oldSource,
    range,
    render,
    Tally,
    topicCases,
} from "./s_tokens_shared.ts";

/// Old fixture → the cases holding its links.
const MOVED: Record<string, { cases: string[] }> = {
    "embed_directives/01_embed.cpp": { cases: ["document_links/embed_directives/01_embed"] },
    "embed_directives/02_has_embed.cpp": {
        cases: ["document_links/embed_directives/02_has_embed"],
    },
    "escapes.cpp": { cases: ["document_links/file_names/01_escaped_characters"] },
    "reserved.cpp": { cases: ["document_links/file_names/01_escaped_characters"] },
    "include_directives/01_quoted_include.cpp": {
        cases: ["document_links/include_directives/02_preamble_split"],
    },
    "include_directives/02_angle_include.cpp": {
        cases: ["document_links/include_directives/01_header_paths"],
    },
    "include_directives/03_macro_include.cpp": {
        cases: ["document_links/include_directives/03_macro_include"],
    },
    "include_directives/04_include_next/main.cpp": {
        cases: ["document_links/include_directives/04_include_next"],
    },
    "include_directives/05_has_include.cpp": {
        cases: ["document_links/include_directives/05_has_include"],
    },
    "macro_has_include.cpp": { cases: ["document_links/include_directives/06_macro_has_include"] },
    "presentation/01_tooltip.cpp": { cases: ["document_links/presentation/01_tooltips"] },
    "module_declarations/01_modules.cpp": {
        cases: ["document_links/module_declarations/01_module_imports"],
    },
};

/// Changed on purpose: the angled include of the old fixture names another
/// header in the case it merged into.
const EXPLAINED: Record<string, string> = {
    "#include ⟦<header_a.h>⟧ -> header_a.h":
        "01_header_paths links its angled include <panel/dial.h> found on -I the same way",
};

const LINK = /^- \{ range: "([^"]+)", target: "(.*)" \}$/;

/// The old target relative to the directory the fixture's files sit in.
function target(raw: string): string {
    return raw
        .replace("${WS}/", "")
        .replace(/^embed_directives\//, "")
        .replace(/^include_directives\/04_include_next\//, "");
}

const tally = new Tally();
const c = corpus("document_links");
for (const fixture of c.fixtures) {
    const moved = MOVED[fixture.rel];
    if (moved === undefined) {
        tally.unexplained.push(`${fixture.rel}: no case`);
        continue;
    }
    if (!fixture.active) {
        continue;
    }
    const old: string[] = [];
    for (const [file, lines] of oldBody(c, fixture)) {
        const source = oldSource(fixture, file === "main.cpp" ? undefined : file);
        for (const line of lines) {
            const match = LINK.exec(line);
            if (match === null) {
                throw new Error(`${fixture.rel}: unread line ${line}`);
            }
            old.push(`${render(source, range(match[1] ?? ""))} -> ${target(match[2] ?? "")}`);
        }
        tally.oldAsked += 2;
    }
    const found: string[] = [];
    for (const id of moved.cases) {
        for (const blocks of newBlocks(id).values()) {
            for (const block of blocks.values()) {
                for (const link of (block["document_links"] ?? []) as {
                    at: string;
                    target: string;
                }[]) {
                    found.push(`${bare(link.at)} -> ${link.target}`);
                }
            }
        }
    }
    tally.check(fixture.rel, old, found, (fact) => EXPLAINED[fact]);
}
for (const id of topicCases("document_links")) {
    tally.newAsked += asked(id);
}
tally.print("document_links");
