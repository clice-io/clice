/// One-off check of the document symbol migration (tests/snap/document_symbol
/// → tests-next/document_symbol): every outline the old snapshots pinned is
/// in the snap.toml of the case it moved to, each symbol's name, kind,
/// selection range, extent and detail as they were, nested the same way.
///
///   npx tsx tools/migrate/document_symbol.ts

import {
    bare,
    corpus,
    newBlocks,
    oldBodies,
    oldSource,
    parseRange,
    renderOld,
    report,
    testOf,
    type Destination,
    type Explained,
    Mapping,
    type NewBlock,
    type Tally,
} from "./s_hover_shared.ts";
import type { SourceText } from "../testing/render/position.ts";

const D = "document_symbol/";

const MAP: Record<string, Destination> = {
    "conversion_names.cpp": { cases: ["symbol_kinds/08_conversion_names"] },
    "explicit_member_instantiation.cpp": { cases: ["symbol_kinds/12_member_instantiations"] },
    "internal_partition.cpp": { cases: ["symbol_kinds/13_internal_partitions"] },
    "location_correctness/01_macro_symbols.cpp": { cases: ["symbol_ranges/03_macro_expansions"] },
    "location_correctness/02_macro_argument_names.cpp": {
        cases: ["symbol_ranges/04_macro_arguments"],
    },
    "missing_symbols/02_local_symbols.cpp": { cases: ["symbol_hierarchy/04_local_symbols"] },
    "missing_symbols/05_friend_definitions.cpp": {
        cases: ["symbol_hierarchy/05_friend_definitions"],
    },
    "ms_property.cpp": { cases: ["symbol_kinds/09_ms_properties"] },
    "symbol_detail/01_detail_signatures.cpp": { cases: ["symbol_detail/01_detail_signatures"] },
    "symbol_detail/02_detail_variable_types.cpp": { cases: ["symbol_detail/02_variable_types"] },
    "symbol_detail/03_detail_default_arguments.cpp": {
        cases: ["symbol_detail/03_default_arguments"],
    },
    "symbol_detail/05_detail_multiline_signatures.cpp": {
        cases: ["symbol_ranges/02_multiline_signatures"],
    },
    "symbol_detail/06_detail_scoped_types.cpp": { cases: ["symbol_detail/05_scoped_types"] },
    "symbol_hierarchy/01_hierarchy_nesting.cpp": {
        cases: ["symbol_hierarchy/02_qualified_definitions"],
    },
    "symbol_hierarchy/02_hierarchy_selection_ranges.cpp": {
        cases: ["symbol_ranges/01_selection_ranges"],
    },
    "symbol_hierarchy/04_hierarchy_anonymous.cpp": {
        cases: ["symbol_hierarchy/03_anonymous_scopes"],
    },
    "symbol_hierarchy/05_hierarchy_utf16.cpp": { cases: ["symbol_ranges/05_utf16_columns"] },
    "symbol_kinds/01_kinds_basic.cpp": { cases: ["symbol_kinds/01_core_kinds"] },
    "symbol_kinds/02_kinds_templates.cpp": { cases: ["symbol_kinds/02_templates"] },
    "symbol_kinds/03_kinds_specializations.cpp": { cases: ["symbol_kinds/03_specializations"] },
    "symbol_kinds/04_kinds_type_aliases.cpp": { cases: ["symbol_kinds/04_type_aliases"] },
    "symbol_kinds/05_kinds_explicit_instantiations.cpp": {
        cases: ["symbol_kinds/05_explicit_instantiations"],
    },
    "symbol_kinds/06_kinds_macros.cpp": { cases: ["symbol_kinds/06_macros"] },
    "symbol_kinds/08_kinds_modules.cpp": { cases: ["symbol_kinds/07_modules"] },
    "unlocated_range_end.cpp": { cases: ["symbol_ranges/06_unlocated_ends"] },
    "unlocated_range_recovery.cpp": { cases: ["symbol_ranges/07_error_recovery"] },
};

/// Views that differ on purpose: `<fixture>@<outline|extents>` → why.
const EXPLAINED: Explained = {
    // The documented case leaves out the fixture's three maintainer comment
    // lines inside the namespace, which spans 46 lines instead of 49; every
    // other symbol is as it was.
    "symbol_kinds/03_kinds_specializations.cpp@extents": "namespace 3 lines shorter",
};

interface OldSymbol {
    name: string;
    kind: string;
    range: string;
    selection: string;
    detail?: string;
    children: OldSymbol[];
}

const STRING = String.raw`"((?:[^"\\]|\\.)*)"`;
const LINE = new RegExp(
    String.raw`^-( +)\{ name: ${STRING}, kind: (\w+), range: "([^"]+)", selection_range: "([^"]+)"(?:, detail: ${STRING})? \}$`,
);

function unquote(text: string): string {
    return JSON.parse(`"${text}"`) as string;
}

/// The outline of one old snapshot body: one symbol a line, nested by
/// indentation.
function parseOutline(lines: string[]): OldSymbol[] {
    const roots: OldSymbol[] = [];
    const stack: OldSymbol[] = [];
    for (const line of lines) {
        if (line === "") {
            continue;
        }
        const m = LINE.exec(line);
        if (m === null) {
            throw new Error(`not an outline line: ${line}`);
        }
        const depth = ((m[1] ?? " ").length - 1) / 2;
        const symbol: OldSymbol = {
            name: unquote(m[2] ?? ""),
            kind: m[3] ?? "",
            range: m[4] ?? "",
            selection: m[5] ?? "",
            ...(m[6] === undefined ? {} : { detail: unquote(m[6]) }),
            children: [],
        };
        stack.length = depth;
        const parent = stack[depth - 1];
        (parent === undefined ? roots : parent.children).push(symbol);
        stack.push(symbol);
    }
    return roots;
}

/// The view of an outline a block shows: its details or its extents.
type View = "detail" | "range";

/// An old symbol as the new output's `view` writes it.
function render(symbol: OldSymbol, source: SourceText, view: View): Record<string, unknown> {
    const detail =
        symbol.detail === undefined || symbol.detail === "" ? {} : { detail: symbol.detail };
    return {
        name: symbol.name,
        kind: symbol.kind,
        at: renderOld(source, parseRange(symbol.selection)),
        ...(view === "detail" ? detail : { range: renderOld(source, parseRange(symbol.range)) }),
        ...(symbol.children.length === 0
            ? {}
            : { children: symbol.children.map((child) => render(child, source, view)) }),
    };
}

/// A new symbol with its places' offsets dropped and its keys in the
/// order render() writes them.
function normalize(symbol: Record<string, unknown>): Record<string, unknown> {
    const children = symbol["children"] as Record<string, unknown>[] | undefined;
    return {
        name: symbol["name"],
        kind: symbol["kind"],
        at: bare(symbol["at"] as string),
        ...(symbol["detail"] === undefined ? {} : { detail: symbol["detail"] }),
        ...(symbol["range"] === undefined ? {} : { range: bare(symbol["range"] as string) }),
        ...(children === undefined ? {} : { children: children.map(normalize) }),
    };
}

/// Count the symbols of a tree.
function count(symbols: readonly { children?: unknown }[]): number {
    return symbols.reduce(
        (sum, symbol) =>
            sum + 1 + count((symbol.children as { children?: unknown }[] | undefined) ?? []),
        0,
    );
}

function main(): void {
    const c = corpus("document_symbol");
    const blocks = newBlocks("document_symbol");
    const tally: Tally = { checked: 0, explained: 0, dropped: 0, unexplained: [] };
    const mapping = new Mapping();
    let oldAsked = 0;
    for (const fixture of c.fixtures) {
        if (!fixture.active) {
            continue;
        }
        const destination = MAP[fixture.rel];
        if (destination === undefined) {
            tally.unexplained.push(`${fixture.rel}: not mapped`);
            continue;
        }
        for (const body of oldBodies(c, fixture)) {
            oldAsked += 2;
            const source = oldSource(fixture, body.file);
            const outline = parseOutline(body.lines);
            const { test } = testOf(body.variant);
            const id = destination.cases[0] ?? "";
            mapping.record(fixture.rel, `${String(count(outline))} symbols`, `${D}${id}`);
            for (const [label, view] of [
                ["outline", "detail"],
                ["extents", "range"],
            ] as const) {
                const want = outline.map((symbol) => render(symbol, source, view));
                const found = (blocks.get(D + id) ?? []).filter(
                    (b: NewBlock) => b.label === label && b.test === test,
                );
                if (found.length !== 1) {
                    tally.unexplained.push(
                        `${fixture.rel}: ${found.length} ${label} blocks in ${id}`,
                    );
                    continue;
                }
                const symbols = (found[0]?.table["document_symbols"] ?? []) as Record<
                    string,
                    unknown
                >[];
                const got = symbols.map(normalize);
                const why = EXPLAINED[`${fixture.rel}@${label}`];
                if (JSON.stringify(got) === JSON.stringify(want)) {
                    tally.checked += count(want);
                } else if (why !== undefined) {
                    tally.explained += count(want);
                } else {
                    tally.unexplained.push(
                        `${fixture.rel} → ${id} ${label}:\n      old ${JSON.stringify(want)}\n      new ${JSON.stringify(got)}`,
                    );
                }
            }
        }
    }
    // An outline is one request a client, shown as two views; a pair
    // pinned apart shows each client's apart.
    let newAsked = 0;
    for (const list of blocks.values()) {
        const outlines = list.filter((block) => block.label === "outline").length;
        newAsked += list.some((block) => block.client !== undefined) ? outlines : outlines * 2;
    }
    report("document_symbol", tally, oldAsked, newAsked);
    mapping.print("old document symbol fixture");
}

main();
