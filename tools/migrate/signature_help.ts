/// One-off check of the signature help migration (tests/snap/signature_help
/// → tests-next/signature_help): every marker's signatures, the active
/// parameter in ⟦⟧, are in the snap.toml of the case it moved to, as they
/// were. The old snapshots did not pin the active signature; the new ones do.
///
///   npx tsx tools/migrate/signature_help.ts

import {
    corpus,
    newBlocks,
    oldBodies,
    renamed,
    report,
    testOf,
    type Destination,
    Mapping,
    type NewBlock,
    type Tally,
} from "./s_hover_shared.ts";

const S = "signature_help/";

const MAP: Record<string, Destination> = {
    "overload_signatures/01_overloads.cpp": { cases: ["overload_signatures/02_overload_lists"] },
    "overload_signatures/02_active_parameter.cpp": {
        cases: ["overload_signatures/03_active_parameter"],
    },
    "overload_signatures/03_member_overloads.cpp": {
        cases: ["overload_signatures/04_const_overloads"],
    },
    "overload_signatures/04_default_arguments.cpp": { cases: ["parameters/01_default_arguments"] },
    "overload_signatures/05_variadic_cstyle.cpp": { cases: ["parameters/02_c_variadics"] },
    "overload_signatures/06_variadic_template.cpp": { cases: ["parameters/03_parameter_packs"] },
    "overload_signatures/07_active_beyond_last.cpp": {
        cases: ["overload_signatures/05_fewer_parameters"],
    },
    "special_call_contexts/01_constructor_aggregate.cpp": {
        cases: ["call_contexts/01_constructors"],
    },
    "special_call_contexts/02_function_pointer.cpp": {
        cases: ["call_contexts/02_function_pointers"],
    },
    "special_call_contexts/03_template_arguments.cpp": {
        cases: ["call_contexts/03_template_arguments"],
    },
    "special_call_contexts/04_nested_calls.cpp": { cases: ["call_contexts/04_nested_calls"] },
    "special_call_contexts/05_operator_call.cpp": { cases: ["call_contexts/05_functors"] },
    "special_call_contexts/06_lambda_call.cpp": { cases: ["call_contexts/06_lambdas"] },
    "special_call_contexts/07_new_expression.cpp": { cases: ["call_contexts/07_new_expressions"] },
    "unicode_parameter.cpp": { cases: ["parameters/04_unicode_names"] },
};

/// Each marker's signature lines: `name:` heads, `- label` items.
function parseSignatures(lines: string[], markers: ReadonlySet<string>): Map<string, string[]> {
    const out = new Map<string, string[]>();
    let current: string[] | undefined;
    for (const line of lines) {
        const head = /^([A-Za-z0-9_]+):$/.exec(line);
        if (head?.[1] !== undefined && markers.has(head[1])) {
            current = [];
            out.set(head[1], current);
            continue;
        }
        if (current === undefined || line === "" || line === "NO SIGNATURES") {
            continue;
        }
        if (line.startsWith("… +")) {
            throw new Error("no old fixture cut its list");
        }
        current.push(line.replace(/^- /, ""));
    }
    return out;
}

function main(): void {
    const c = corpus("signature_help");
    const blocks = newBlocks("signature_help");
    const tally: Tally = { checked: 0, explained: 0, dropped: 0, unexplained: [] };
    const mapping = new Mapping();
    let oldAsked = 0;
    for (const fixture of c.fixtures) {
        const destination = MAP[fixture.rel];
        if (destination === undefined) {
            tally.unexplained.push(`${fixture.rel}: not mapped`);
            continue;
        }
        for (const body of oldBodies(c, fixture)) {
            const markers = new Set(fixture.files.flatMap((f) => [...f.source.offsets.keys()]));
            for (const [marker, labels] of parseSignatures(body.lines, markers)) {
                oldAsked += 2;
                const label = destination.renames?.[marker] ?? renamed(marker);
                const { test } = testOf(body.variant);
                const found: NewBlock[] = destination.cases.flatMap((id) =>
                    (blocks.get(S + id) ?? []).filter((b) => b.label === label && b.test === test),
                );
                const key = `${fixture.rel}#${marker}`;
                const [block] = found;
                if (block === undefined || found.length !== 1) {
                    tally.unexplained.push(`${key}: ${found.length} blocks named ${label}`);
                    continue;
                }
                mapping.record(fixture.rel, marker, block.caseId);
                const help = block.table["signature_help"] as { signatures?: string[] } | undefined;
                const got = help?.signatures ?? [];
                if (JSON.stringify(got) === JSON.stringify(labels)) {
                    tally.checked += 1;
                } else {
                    tally.unexplained.push(
                        `${key} → ${block.caseId}: old ${JSON.stringify(labels)}, new ${JSON.stringify(got)}`,
                    );
                }
            }
        }
    }
    let newAsked = 0;
    for (const list of blocks.values()) {
        newAsked += list.filter((block) => "signature_help" in block.table).length * 2;
    }
    report("signature_help", tally, oldAsked, newAsked);
    mapping.print("old signature help fixture");
}

main();
