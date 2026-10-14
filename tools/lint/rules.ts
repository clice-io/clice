/// The repository's own ESLint rules, for the TypeScript of tools/, tests/
/// and editors/vscode: `import clice from "@clice/tools/lint"`, then the
/// rules as `clice/<name>`.

import type { Rule } from "eslint";
import type * as ESTree from "estree";

/// The kinds of timer that stand in for no event, as the comment allowing
/// one names them; scripts/check_timers.py takes the same for the C++.
const LABELS = ["Hang detection:", "Time is the feature:", "No event:"];

const TIMER_FUNCTIONS = new Set(["setTimeout", "setInterval"]);
/// A hang bound shorter than this fails a test on a loaded machine.
const SHORTEST_BOUND = 60_000;
const TIMER_MODULES = new Set(["timers", "timers/promises", "node:timers", "node:timers/promises"]);

/// A wait on the clock where an event exists makes a test pass or fail by
/// the machine's load. A timer is allowed where a comment right before it,
/// or before a statement or declaration around it, starts with a label:
/// `Hang detection: <why>` for a bound that only a hang reaches, `Time is
/// the feature: <why>` where the time itself is what the code does (a
/// pacing, a grace period, a test whose claim is about time), `No event:
/// <why>` for a condition re-checked until it holds, as nothing announces
/// it. A literal bound of `withTimeout` (tools/promise.ts) shorter than a
/// minute needs one too.
const timers: Rule.RuleModule = {
    meta: {
        type: "problem",
        docs: { description: "Disallow timers that stand in for an event" },
        messages: {
            unlabelled:
                "A timer waiting for something that has an event fails on a loaded machine: " +
                "wait on the event. Any other timer follows a comment " +
                "`Hang detection: <why>`, `Time is the feature: <why>` or `No event: <why>`.",
            shortBound:
                "A hang bound under a minute fails on a loaded machine: make it a minute or " +
                "more, or say why it is not one in a comment `Time is the feature: <why>`.",
        },
        schema: [],
    },
    create(context) {
        const source = context.sourceCode;
        const labelled = (node: ESTree.Node): boolean =>
            [node, ...source.getAncestors(node)]
                .filter((each) => each.type !== "Program")
                .some((each) =>
                    source.getCommentsBefore(each).some((comment) => {
                        const text = comment.value.replace(/^[/*\s]+/, "");
                        return LABELS.some(
                            (label) => text.startsWith(label) && text.length > label.length + 1,
                        );
                    }),
                );
        const check = (node: ESTree.Node, messageId = "unlabelled"): void => {
            if (!labelled(node)) {
                context.report({ node, messageId });
            }
        };
        return {
            CallExpression(node) {
                const callee = node.callee;
                const name =
                    callee.type === "Identifier"
                        ? callee.name
                        : callee.type === "MemberExpression" &&
                            callee.property.type === "Identifier"
                          ? callee.property.name
                          : undefined;
                if (name !== undefined && TIMER_FUNCTIONS.has(name)) {
                    check(node);
                }
                const bound = node.arguments[1];
                if (
                    name === "withTimeout" &&
                    bound?.type === "Literal" &&
                    typeof bound.value === "number" &&
                    bound.value < SHORTEST_BOUND
                ) {
                    check(node, "shortBound");
                }
            },
            ImportDeclaration(node) {
                if (TIMER_MODULES.has(String(node.source.value))) {
                    check(node);
                }
            },
        };
    },
};

export default { rules: { timers } };
