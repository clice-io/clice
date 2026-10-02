/// Real build systems and real toolchains: each scenario builds the
/// shared project, then clice must parse every unit of the database the
/// build wrote as cleanly as the compiler did, agree with the compiler on
/// the macros its flags imply, and resolve each file's command as the
/// scenario expects.

import { checkScenario } from "@clice/tools/compat/check";
import { missingTools } from "@clice/tools/compat/scenario";
import { cliceExecutable } from "@clice/tools/session";
import { expect, test } from "vitest";
import { SCENARIOS } from "./scenarios.ts";

for (const scenario of SCENARIOS) {
    // CI installs every tool, so a missing one there is a broken setup
    // that must fail rather than quietly shrink the matrix.
    const skip =
        !scenario.platforms.includes(process.platform) ||
        (missingTools(scenario).length > 0 && process.env["CI"] === undefined);
    test.skipIf(skip)(`${scenario.name} (${scenario.platforms.join(", ")})`, () => {
        // checkScenario throws with everything clice got wrong.
        expect(() => {
            checkScenario(cliceExecutable(), scenario);
        }).not.toThrow();
    });
}
