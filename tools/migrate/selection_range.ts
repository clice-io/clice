/// One-off check of the selection_range migration: every marker of an old
/// fixture keeps its name in the case it moved to, and its chain of ranges,
/// rendered the way the new output names them, is the same, step by step.
///
///     node tools/migrate/selection_range.ts

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

const MOVED: Record<string, string> = {
    "cursor_placement.cpp": "selection_range/cursor_placement/01_between_tokens",
    "inactive_region.cpp": "selection_range/conditionals/01_inactive_branch",
    "unbalanced_conditional.cpp": "selection_range/conditionals/02_unbalanced_branches",
    "syntax_ranges/01_expression_nesting.cpp": "selection_range/syntax_ranges/01_nested_expression",
    "syntax_ranges/02_statement_semicolon.cpp": "selection_range/syntax_ranges/02_statements",
    "syntax_ranges/03_argument_lists.cpp": "selection_range/syntax_ranges/03_argument_lists",
    "syntax_ranges/04_template_lists.cpp": "selection_range/syntax_ranges/04_template_lists",
    "syntax_ranges/05_macro_arguments.cpp": "selection_range/syntax_ranges/05_macro_arguments",
    "syntax_ranges/06_lambdas.cpp": "selection_range/syntax_ranges/06_lambdas",
    "syntax_ranges/07_blocks.cpp": "selection_range/syntax_ranges/07_blocks",
    "text_ranges/01_literals.cpp": "selection_range/text_ranges/01_literals",
    "text_ranges/02_comments.cpp": "selection_range/text_ranges/02_comments",
    "text_ranges/03_directives/main.cpp": "selection_range/text_ranges/03_directives",
};

/// The old 01_expression_nesting merged into the case 01_nested_expression
/// already held: its two markers became that case's `operand` (an operand
/// inside parentheses) and `halved` (a plain operand of a nested binary
/// expression), on that case's code.
const RENAMED: Record<string, string> = {
    "syntax_ranges/01_expression_nesting.cpp": "steps on the code of 01_nested_expression",
};

const STEP = /^ {2}- \{ range: "([^"]+)", text: .* \}$/;

const tally = new Tally();
const c = corpus("selection_range");
for (const fixture of c.fixtures) {
    const id = MOVED[fixture.rel];
    if (id === undefined) {
        tally.unexplained.push(`${fixture.rel}: no case`);
        continue;
    }
    const source = oldSource(fixture);
    const old: string[] = [];
    let marker = "";
    for (const line of oldBody(c, fixture).get(source.path) ?? []) {
        const head = /^(\w+):$/.exec(line);
        if (head !== null) {
            marker = head[1] ?? "";
            tally.oldAsked += 2;
            continue;
        }
        const step = STEP.exec(line);
        if (step === null) {
            throw new Error(`${fixture.rel}: unread line ${line}`);
        }
        old.push(`${marker}: ${render(source, range(step[1] ?? ""))}`);
    }
    const found: string[] = [];
    for (const [label, block] of newBlocks(id).get("answers") ?? []) {
        for (const step of (block["selection_ranges"] ?? []) as string[]) {
            found.push(`${label}: ${bare(step)}`);
        }
    }
    const reason = RENAMED[fixture.rel];
    tally.check(fixture.rel, old, found, () => reason);
}
for (const id of topicCases("selection_range")) {
    tally.newAsked += asked(id);
}
tally.print("selection_range");
