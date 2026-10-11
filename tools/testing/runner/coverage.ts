/// The product surface the suites left unexercised. Runs after serve, features
/// and cli, each run with the same CLICE_COVERAGE_DIR.

import * as path from "node:path";
import { expect, test } from "vitest";
import { cliceExecutable } from "@clice/tools/session";
import { CASES_DIR } from "@clice/tools/testing/cases/project";
import { check } from "@clice/tools/testing/coverage/check";

test("surface covered", async () => {
    const dir = process.env["CLICE_COVERAGE_DIR"];
    expect(dir, "CLICE_COVERAGE_DIR names the records of the suite runs").toBeTruthy();
    const gaps = await check(cliceExecutable(), dir ?? "", path.join(CASES_DIR, "coverage.json"));
    expect(gaps).toEqual([]);
});
