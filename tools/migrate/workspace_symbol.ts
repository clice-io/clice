/// One-off check of the workspace_symbol snap corpus migration (S-NAV):
/// every symbol an old snapshot pinned for a query is in the snap.toml of
/// the case its fixture moved to, rendered the way the new output names it,
/// and the new answer holds nothing the old one did not, unless EXPLAINED
/// says why. Run it from the repository:
/// `node tools/migrate/workspace_symbol.ts`.

import { corpus, missing, newBlocks, oldBlocks, OldFiles, stripOffset } from "./s_nav_shared.ts";

/// Fixture → case; `test` is the test its queries moved to.
const PLAN: Record<string, { to: string; test: string }> = {
    "workspace_symbol/01_basic_search.cpp": { to: "queries/02_kinds_and_case", test: "answers" },
    "workspace_symbol/02_cross_file_search/main.cpp": {
        to: "projects/01_closed_files",
        test: "test",
    },
    "workspace_symbol/03_overload_params.cpp": { to: "results/01_overloads", test: "answers" },
    "workspace_symbol/04_fuzzy_matching.cpp": { to: "queries/03_fuzzy_matching", test: "answers" },
    "workspace_symbol/05_qualified_search.cpp": {
        to: "scopes/01_qualified_names",
        test: "answers",
    },
    "workspace_symbol/06_enum_scope.cpp": { to: "scopes/02_enumerators", test: "answers" },
    "workspace_symbol/07_alias_priority.cpp": { to: "ranking/01_exact_name", test: "test" },
    "workspace_symbol/08_mangled_name.cpp": { to: "queries/05_mangled_names", test: "answers" },
    "workspace_symbol/09_query_syntax.cpp": { to: "queries/04_query_syntax", test: "answers" },
    "aliases/main.cpp": { to: "results/02_aliases", test: "test" },
    "search_noise/main.cpp": { to: "results/03_search_noise", test: "test" },
};

/// `case query` → why the new answer differs on purpose.
const EXPLAINED: Record<string, string> = {
    "projects/01_closed_files helper_elsewhere":
        "the closed file is a source now, other.cpp: the old other.h was a database entry of its own, which no project has",
};

function main(): void {
    const c = corpus("workspace_symbol");
    let asked = 0;
    let unexplained = 0;
    const used = new Set<string>();
    for (const fixture of c.fixtures) {
        const step = PLAN[fixture.rel];
        if (step === undefined) {
            throw new Error(`unplanned fixture ${fixture.rel}`);
        }
        if (!fixture.active) {
            continue;
        }
        const files = new OldFiles(c, fixture);
        const blocks = newBlocks(`workspace_symbol/${step.to}`).get(step.test);
        for (const block of oldBlocks(c, fixture)) {
            asked += 1;
            const key = `${step.to} ${block.name}`;
            const table = blocks?.get(block.name);
            const problems: string[] = [];
            if (table === undefined) {
                problems.push("no block");
            } else {
                const old = block.entries.map(({ fields }) =>
                    JSON.stringify({
                        name: fields["name"],
                        kind: fields["kind"],
                        at: files.place("", fields["file"] ?? "", fields["range"] ?? ""),
                        ...(fields["container"] === undefined
                            ? {}
                            : { container: fields["container"] }),
                    }),
                );
                const have = ((table["workspace_symbols"] ?? []) as Record<string, string>[]).map(
                    (symbol) => JSON.stringify({ ...symbol, at: stripOffset(symbol["at"] ?? "") }),
                );
                problems.push(
                    ...missing(old, have).map((entry) => `lost ${entry}`),
                    ...missing(have, old).map((entry) => `gained ${entry}`),
                );
            }
            if (problems.length === 0) {
                continue;
            }
            if (EXPLAINED[key] !== undefined) {
                used.add(key);
                continue;
            }
            unexplained += 1;
            console.log(`${fixture.rel} → ${key}\n  ${problems.join("\n  ")}`);
        }
    }
    for (const key of Object.keys(EXPLAINED)) {
        if (!used.has(key)) {
            console.log(`explained but no difference: ${key}`);
            unexplained += 1;
        }
    }
    console.log(`old: ${String(asked)} queries`);
    console.log(`unexplained differences: ${String(unexplained)}`);
    process.exitCode = unexplained === 0 ? 0 : 1;
}

main();
