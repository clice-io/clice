/// Real build systems and real toolchains: each scenario builds the
/// shared project, then clice must parse every unit of the database the
/// build wrote as cleanly as the compiler did, agree with the compiler on
/// the macros its flags imply, and resolve each file's command as the
/// scenario expects.

import { checkBuild, checkScenario } from "@clice/tools/compat/check";
import { missingTools } from "@clice/tools/compat/scenario";
import { cliceExecutable } from "@clice/tools/session";
import { expect, test } from "vitest";
import { SCENARIOS } from "./scenarios.ts";

for (const scenario of SCENARIOS) {
    const platforms = scenario.platforms.join(", ");
    // CI installs every tool, so a missing one there is a broken setup
    // that must fail rather than quietly shrink the matrix.
    const skip =
        !scenario.platforms.includes(process.platform) ||
        (missingTools(scenario).length > 0 && process.env["CI"] === undefined);
    test.skipIf(skip || scenario.unsupported !== undefined)(
        `${scenario.name} (${platforms})`,
        async () => {
            // checkScenario rejects with everything clice got wrong.
            await expect(checkScenario(cliceExecutable(), scenario)).resolves.toBeUndefined();
        },
    );
    if (scenario.unsupported !== undefined) {
        test.skipIf(skip)(`${scenario.name} (${platforms}) builds`, async () => {
            await expect(checkBuild(scenario)).resolves.toBeUndefined();
        });
    }
}
