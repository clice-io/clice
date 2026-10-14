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
/// <why>` for what nothing announces: a condition re-checked until it
/// holds, or an end. A timer imported from a timers module under another
/// name is a timer too. A literal bound of `withTimeout` (tools/promise.ts)
/// shorter than a minute is no hang bound: only `Time is the feature:`
/// allows it.
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
        const labelled = (node: ESTree.Node, labels: readonly string[]): boolean =>
            [node, ...source.getAncestors(node)]
                .filter((each) => each.type !== "Program")
                .some((each) =>
                    source.getCommentsBefore(each).some((comment) => {
                        const text = comment.value.replace(/^[/*\s]+/, "");
                        return labels.some(
                            (label) => text.startsWith(label) && text.length > label.length + 1,
                        );
                    }),
                );
        // The local names of the timers a timers module gave the file;
        // imports are hoisted, so the calls are judged once all are seen.
        const imported = new Set<string>();
        const schedulers = new Set<string>();
        const calls: ESTree.CallExpression[] = [];
        const timer = (callee: ESTree.Node): boolean => {
            if (callee.type === "Identifier") {
                return TIMER_FUNCTIONS.has(callee.name) || imported.has(callee.name);
            }
            if (callee.type !== "MemberExpression" || callee.property.type !== "Identifier") {
                return false;
            }
            if (TIMER_FUNCTIONS.has(callee.property.name)) {
                return true;
            }
            const object = callee.object;
            return (
                callee.property.name === "wait" &&
                ((object.type === "Identifier" && schedulers.has(object.name)) ||
                    (object.type === "MemberExpression" &&
                        object.property.type === "Identifier" &&
                        object.property.name === "scheduler"))
            );
        };
        return {
            ImportDeclaration(node) {
                if (!TIMER_MODULES.has(String(node.source.value))) {
                    return;
                }
                for (const specifier of node.specifiers) {
                    if (specifier.type !== "ImportSpecifier") {
                        continue;
                    }
                    const name =
                        specifier.imported.type === "Identifier"
                            ? specifier.imported.name
                            : String(specifier.imported.value);
                    if (TIMER_FUNCTIONS.has(name)) {
                        imported.add(specifier.local.name);
                    } else if (name === "scheduler") {
                        schedulers.add(specifier.local.name);
                    }
                }
            },
            CallExpression(node) {
                calls.push(node);
                const callee = node.callee;
                const name =
                    callee.type === "Identifier"
                        ? callee.name
                        : callee.type === "MemberExpression" &&
                            callee.property.type === "Identifier"
                          ? callee.property.name
                          : undefined;
                const bound = node.arguments[1];
                if (
                    name === "withTimeout" &&
                    bound?.type === "Literal" &&
                    typeof bound.value === "number" &&
                    bound.value < SHORTEST_BOUND &&
                    !labelled(node, ["Time is the feature:"])
                ) {
                    context.report({ node, messageId: "shortBound" });
                }
            },
            "Program:exit"() {
                for (const call of calls) {
                    if (timer(call.callee) && !labelled(call, LABELS)) {
                        context.report({ node: call, messageId: "unlabelled" });
                    }
                }
            },
        };
    },
};

export default { rules: { timers } };
