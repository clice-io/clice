/// The editors' fixtures, tests/editors/NN_name: a small project each and
/// the scenario every editor client's end-to-end test runs on it
/// (scenario.ts). They are no cases: the case runner never reads
/// tests/editors (topics.ts). An editor's tests run on copies, which
/// `prepareFixtures` makes.

import * as fs from "node:fs";
import * as path from "node:path";
import { parse } from "smol-toml";
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

const TEST_HOOKS = "[project]\ntest_hooks = true\n";

/// Copy every fixture to `target`/<name>, replacing what `target` held, as
/// a case's copy is made — its databases written —, with scenario.json
/// beside the inputs and the test hooks on in its clice.toml: the harnesses
/// wait on clice/internal/sync. Returns the copies.
export function prepareFixtures(target: string, root = FIXTURES_DIR): string[] {
    const fixtures = loadFixtures(root);
    fs.rmSync(target, { recursive: true, force: true });
    return fixtures.map((fixture) => {
        const copy = path.join(target, fixture.name);
        fs.mkdirSync(copy, { recursive: true });
        const workspace = fs.realpathSync.native(copy);
        writeDatabases(readProject(fixture.dir), materialize(fixture.dir, workspace), workspace);
        const config = path.join(workspace, "clice.toml");
        if (fs.existsSync(config)) {
            const own = fs.readFileSync(config, "utf8");
            if (Object.hasOwn(parse(own), "project")) {
                throw new Error(
                    `${path.join(fixture.dir, "clice.toml")}: [project] is the copy's, which turns the test hooks on`,
                );
            }
            fs.writeFileSync(config, `${own}\n${TEST_HOOKS}`);
        } else {
            fs.writeFileSync(config, TEST_HOOKS);
        }
        return workspace;
    });
}
