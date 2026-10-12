import { execFileSync } from "node:child_process";
import * as os from "node:os";
import { test } from "@clice/tools/testing";

// The churn annotation reads git history, which no case input can carry:
// the test commits the sources itself, with no user or system git
// configuration in the way.
function commit(root: string): void {
    const git = (...args: string[]): void => {
        execFileSync(
            "git",
            ["-c", "user.name=clice", "-c", "user.email=clice@example.com", ...args],
            {
                cwd: root,
                stdio: "ignore",
                env: { ...process.env, GIT_CONFIG_GLOBAL: os.devNull, GIT_CONFIG_NOSYSTEM: "1" },
            },
        );
    };
    git("init", "-q");
    git("add", "core", "util", "app");
    git("commit", "-q", "-m", "one");
}

test(async (t) => {
    await t.cli("index");
    commit(t.root);
    await t.cli("analyze modules --view impact");
    await t.cli("analyze modules --view impact --churn-since=");
});
