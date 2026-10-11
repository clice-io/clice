/// One-off check of the folding_range migration: every fold an old snapshot
/// pinned is in the case it moved to, rendered the way the new output names
/// it, and the folds tests/integration/features/folding_range.test.ts
/// expected in both client modes are those its cases show.
///
///     node tools/migrate/folding_range.ts

import * as fs from "node:fs";
import * as path from "node:path";
import { scanFixtureHeader } from "../snap/corpus.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { resolveLocation, SourceText } from "../testing/render/position.ts";
import {
    asked,
    bare,
    caseSource,
    corpus,
    newBlocks,
    oldBody,
    oldSource,
    range,
    render,
    Tally,
    topicCases,
} from "./s_tokens_shared.ts";

const MOVED: Record<string, string> = {
    "delayed_template_parsing.cpp": "blocks/10_delayed_template_parsing",
    "fold_kinds/01_block_folding.cpp": "blocks/02_block_kinds",
    "fold_kinds/02_nested_compound_statement.cpp": "blocks/03_statement_bodies",
    "fold_kinds/03_multiline_list_folding.cpp": "lists/01_multiline_lists",
    "fold_kinds/04_access_specifier_folding.cpp": "sections/01_access_specifiers",
    "fold_kinds/05_preprocessor_conditional.cpp": "conditionals/01_branches",
    "fold_kinds/06_pragma_region.cpp": "regions/01_pragma_regions",
    "fold_kinds/07_pragma_classification.cpp": "regions/02_pragma_arguments",
    "fold_kinds/08_comment_folding.cpp": "runs/01_comments",
    "fold_kinds/09_include_region/main.cpp": "runs/02_includes",
    "fold_kinds/10_raw_string_literal.cpp": "raw_strings/01_raw_string_literals",
    "fold_kinds/11_using_declaration_block.cpp": "runs/03_using_declarations",
    "fold_kinds/12_template_parameter_list.cpp": "lists/02_template_parameters",
    "fold_kinds/13_template_instantiations.cpp": "blocks/06_template_instantiations",
    "fold_kinds/14_abbreviated_function_template.cpp": "blocks/07_abbreviated_templates",
    "fold_kinds/15_macro_folding.cpp": "macros/01_macro_spelled_blocks",
    "fold_kinds/16_coroutine_body.cpp": "blocks/05_coroutine_bodies",
    "fold_kinds/17_initializer_list_construction.cpp": "blocks/08_initializer_lists",
    "fold_kinds/18_macro_argument_folding.cpp": "macros/02_macro_arguments",
    "fold_kinds/19_linkage_specification.cpp": "blocks/04_linkage_specifications",
    "fold_kinds/20_module_blocks.cpp": "modules/01_module_blocks",
    "global_anonymous_union.cpp": "blocks/09_anonymous_unions",
    "global_fragment_before_module.cpp": "modules/02_fragment_before_partition",
    "nested_conditional.cpp": "conditionals/03_nested_conditionals",
    "parameter_list_bounds.cpp": "lists/03_parameter_list_bounds",
    "refinements/01_collapsed_text.cpp": "presentation/01_collapsed_text",
    "refinements/02_fold_from_declaration_line.cpp": "line_folding/01_declaration_lines",
    "refinements/03_inactive_preprocessor_branch.cpp": "conditionals/02_untaken_branches",
    "refinements/04_single_line_constructs.cpp": "presentation/02_single_line_constructs",
    "taken_branch_else.cpp": "conditionals/04_skipped_chains",
};

/// The comments a documented case dropped: the box drawings that pointed at
/// the folds, and notes that now live in the case's README.
const DROPPED = /\s*\/\/ ([┐│┘]|a lone include stays unfolded).*$/;

/// The old fixture text as the case keeps it: dropped trailing comments
/// gone, so a fold starting past one starts at the end of its line.
function kept(source: SourceText): SourceText {
    return new SourceText(
        source.path,
        source.lines.map((line) => line.replace(DROPPED, "")).join("\n"),
    );
}

/// Folds the case changed on purpose.
const CHANGED: Record<string, string> = {
    "functionBody void process(int count) ⟦{ … }⟧ (35 lines) {...}":
        "the body lost the three comment lines that moved to the README",
};

const FOLD = /^- \{ range: "([^"]+)", kind: (\w+)(?:, collapsed_text: (".*"))? \}$/;

const tally = new Tally();
const c = corpus("folding_range");
for (const fixture of c.fixtures) {
    const moved = MOVED[fixture.rel];
    if (moved === undefined) {
        tally.unexplained.push(`${fixture.rel}: no case`);
        continue;
    }
    const id = `folding_range/${moved}`;
    const original = oldSource(fixture);
    const source = kept(original);
    const header = scanFixtureHeader(fixture.files[0]?.content ?? "");
    const caseText = caseSource(id).source.text;
    const old: string[] = [];
    const gone = new Set<string>();
    for (const line of oldBody(c, fixture).get(original.path) ?? []) {
        const match = FOLD.exec(line);
        if (match === null) {
            throw new Error(`${fixture.rel}: unread line ${line}`);
        }
        const r = range(match[1] ?? "");
        for (const end of [r.start, r.end]) {
            end.character = Math.min(end.character, source.line(end.line).length);
        }
        const collapsed = match[3] === undefined ? "" : (JSON.parse(match[3]) as string);
        const fact = `${match[2] ?? ""} ${render(source, r)} ${collapsed}`;
        old.push(fact);
        // A comment the case no longer holds: the old doc header, its
        // `// snap:` notes, or a maintainer note now in the README.
        const first = source.line(r.start.line).trim();
        if (
            match[2] === "comment" &&
            (r.start.line < header.bodyStart || !caseText.includes(first))
        ) {
            gone.add(fact);
        }
    }
    tally.oldAsked += 2;
    const found: string[] = [];
    for (const [test, blocks] of newBlocks(id)) {
        if (test === "whole lines") {
            continue;
        }
        for (const block of blocks.values()) {
            for (const fold of (block["folding_ranges"] ?? []) as Record<string, string>[]) {
                found.push(
                    `${fold["kind"] ?? ""} ${bare(fold["at"] ?? "")} ${fold["collapsed"] ?? ""}`,
                );
            }
        }
    }
    tally.check(fixture.rel, old, found, (fact) =>
        gone.has(fact) ? "folded a comment the case no longer holds" : CHANGED[fact],
    );
}

// The integration test's three sources, each in both client modes.
const INTEGRATION = path.join(CASES_DIR, "../tests/integration/features/folding_range.test.ts");
const LINE_CASES = [
    "line_folding/02_blocks_and_sections",
    "line_folding/03_runs_and_delimiters",
    "line_folding/04_declaration_styles",
];
const expected = [
    ...fs.readFileSync(INTEGRATION, "utf8").matchAll(/(lines|characters): \[([^\]]*)\]/g),
];
for (const [index, id] of LINE_CASES.entries()) {
    const { source } = caseSource(`folding_range/${id}`);
    const scope = { file: () => source, query: { path: source.path } };
    for (const [mode, test] of [
        ["lines", "whole lines"],
        ["characters", "characters"],
    ] as const) {
        const list = expected.find((m, i) => m[1] === mode && Math.floor(i / 2) === index)?.[2];
        const want = [...(list ?? "").matchAll(/"([^"]+)"/g)].map((m) => m[1] ?? "");
        const got: string[] = [];
        for (const block of newBlocks(`folding_range/${id}`).get(test)?.values() ?? []) {
            for (const fold of (block["folding_ranges"] ?? []) as Record<string, string>[]) {
                const { start, end } = resolveLocation(fold["at"] ?? "", scope).range;
                got.push(
                    mode === "lines"
                        ? `${start.line}:--${end.line}:- ${fold["kind"] ?? ""}`
                        : `${start.line}:${start.character}-${end.line}:${end.character} ${fold["kind"] ?? ""}`,
                );
            }
        }
        tally.check(`integration ${id} ${mode}`, want, got, () => undefined);
        tally.check(`integration ${id} ${mode} (extra)`, got, want, () => undefined);
    }
}

for (const id of topicCases("folding_range")) {
    tally.newAsked += asked(id);
}
tally.print("folding_range");
