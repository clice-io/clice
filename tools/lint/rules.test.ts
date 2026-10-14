/// Tests for the repository's own ESLint rules (tools/lint/rules.ts).

import { RuleTester } from "eslint";
import tseslint from "typescript-eslint";
import { describe, it } from "vitest";
import clice from "./rules.ts";

RuleTester.describe = describe;
RuleTester.it = it;
RuleTester.itOnly = it.only;

const tester = new RuleTester({ languageOptions: { parser: tseslint.parser } });

tester.run("timers", clice.rules.timers, {
    valid: [
        "const later = setImmediate(() => undefined);",
        "// Hang detection: a server that never answers.\nconst timer = setTimeout(fail, 60_000);",
        "/// Time is the feature: the pacing of a recorded trace.\nfunction pace(ms: number) {\n" +
            "    return new Promise((resolve) => setTimeout(resolve, ms));\n}",
        "// Hang detection: the connect never finishes.\nsocket.setTimeout(5000, fail);",
        "// No event: the log is read again.\nawait new Promise((resolve) => setTimeout(resolve, 1000));",
        "const timer = {\n    // Time is the feature: a grace period.\n    id: setInterval(tick, 10),\n};",
        "await withTimeout(exited, 60_000);",
        "await withTimeout(exited, bound);",
        "// Time is the feature: the timeout itself is tested.\nawait withTimeout(hang, 5);",
    ],
    invalid: [
        {
            code: "await new Promise((resolve) => setTimeout(resolve, 200));",
            errors: [{ messageId: "unlabelled" }],
        },
        {
            code: "// Waits for the selection.\nconst timer = setTimeout(done, 200);",
            errors: [{ messageId: "unlabelled" }],
        },
        {
            code: "// Hang detection:\nconst timer = setTimeout(done, 200);",
            errors: [{ messageId: "unlabelled" }],
        },
        {
            code: "// Time is the feature: the whole file.\n\nimport { x } from './x';\nsetInterval(poll, 1000);",
            errors: [{ messageId: "unlabelled" }],
        },
        {
            code: "globalThis.setTimeout(poll, 1000);",
            errors: [{ messageId: "unlabelled" }],
        },
        {
            code: 'import { setTimeout as delay } from "node:timers/promises";',
            errors: [{ messageId: "unlabelled" }],
        },
        {
            code: 'await withTimeout(stderrEof, 2_000, "stderr EOF");',
            errors: [{ messageId: "shortBound" }],
        },
        {
            code: '// Hang detection: EOF follows the exit.\nawait withTimeout(stderrEof, 2_000, "stderr EOF");',
            errors: [{ messageId: "shortBound" }],
        },
    ],
});
