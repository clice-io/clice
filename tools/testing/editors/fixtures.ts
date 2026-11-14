/// The editors' fixtures, tests/editors/NN_name: a small project each and
/// the scenario every editor client's end-to-end test runs on it
/// (scenario.ts). They are no cases: the case runner never reads
/// tests/editors (topics.ts). An editor's tests run on copies, which
/// `prepareFixtures` makes.

import * as fs from "node:fs";
import * as path from "node:path";
import { CASES_DIR, materialize, readProject, writeDatabases } from "../cases/project.ts";
import { readScenario } from "./scenario.ts";

export const FIXTURES_DIR = path.join(CASES_DIR, "editors");

/// The suite beside the fixtures: the Zed extension in a real Zed.
const SUITE = "zed";
const FIXTURE = /^\d\d_[a-z0-9]+(?:_[a-z0-9]+)*$/;

export interface Fixture {
    /// The directory's name, and its copy's.
    name: string;
    dir: string;
}

/// The fixtures under `root`, in their order; each scenario is checked.
export function loadFixtures(root = FIXTURES_DIR): Fixture[] {
    const fixtures: Fixture[] = [];
    for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
        if (!entry.isDirectory() || entry.name === SUITE) {
            continue;
        }
        const dir = path.join(root, entry.name);
        if (!FIXTURE.test(entry.name)) {
            throw new Error(
                `${dir}: a fixture is named NN_words, two digits, lowercase words joined by _`,
            );
        }
        readScenario(dir);
        fixtures.push({ name: entry.name, dir });
    }
    return fixtures.sort((a, b) => a.name.localeCompare(b.name));
}

/// Copy every fixture to `target`/<name>, replacing what `target` held, as
/// a case's copy is made — its databases written —, with scenario.json
/// beside the inputs. The editors run their servers with the test hooks on
/// (CLICE_TEST_HOOKS, pixi.toml): the harnesses wait on
/// clice/internal/sync. Returns the copies.
export function prepareFixtures(target: string, root = FIXTURES_DIR): string[] {
    const fixtures = loadFixtures(root);
    fs.rmSync(target, { recursive: true, force: true });
    return fixtures.map((fixture) => {
        const copy = path.join(target, fixture.name);
        fs.mkdirSync(copy, { recursive: true });
        const workspace = fs.realpathSync.native(copy);
        writeDatabases(readProject(fixture.dir), materialize(fixture.dir, workspace), workspace);
        return workspace;
    });
}
