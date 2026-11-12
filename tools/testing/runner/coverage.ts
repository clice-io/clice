/// The product surface the cases left unexercised: runs after them, over
/// the records every shard wrote under CLICE_COVERAGE_DIR.

import * as path from "node:path";
import { expect, test } from "vitest";
import { cliceExecutable } from "@clice/tools/session";
import { CASES_DIR } from "@clice/tools/testing/cases/project";
import { check } from "@clice/tools/testing/coverage/check";

test("surface covered", async () => {
    const dir = process.env["CLICE_COVERAGE_DIR"];
    expect(dir, "CLICE_COVERAGE_DIR names the records of the suite runs").toBeTruthy();
    const gaps = await check(cliceExecutable(), dir ?? "", path.join(CASES_DIR, "coverage.toml"));
    expect(gaps).toEqual([]);
});
