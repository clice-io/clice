// Bazel's --workspace_status_command: STABLE_CLICE_VERSION, the version
// version.cpp is stamped with (bazel/BUILD.bazel). A change of it rebuilds
// version.cpp alone and relinks the programs.
//
// The version is `git describe` relative to the latest tag, without its v
// prefix; a tag-exact build prints the tag itself; without a reachable tag
// it is the base version plus the commit hash, and outside a git checkout of
// clice (a source tarball) the base version alone.

import { execFileSync } from "node:child_process";
import { realpathSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const BASE_VERSION = "0.1.0";

const root = realpathSync(resolve(dirname(fileURLToPath(import.meta.url)), ".."));

/// git's output, or null when it fails. A git that hangs (seen on Windows
/// runners) fails the build rather than stall it.
function git(...args) {
    try {
        return execFileSync("git", ["-C", root, ...args], {
            encoding: "utf8",
            stdio: ["ignore", "pipe", "ignore"],
            timeout: 60_000,
        }).trim();
    } catch (error) {
        if (error.code === "ETIMEDOUT") {
            throw new Error(`git ${args.join(" ")} did not finish in a minute`);
        }
        return null;
    }
}

function describe() {
    // git finds the repository of a parent directory too: a source tarball
    // unpacked inside some other checkout must not describe that one.
    const toplevel = git("rev-parse", "--show-toplevel");
    if (!toplevel || realpathSync(toplevel) !== root) {
        return null;
    }
    // A commit can carry several tags (a stable tag placed on a nightly's
    // commit); describe alone picks one of them, so take the highest.
    const tag = git("tag", "--points-at", "HEAD", "--sort=-v:refname")?.split("\n")[0];
    if (tag) {
        // As describe --dirty: a modified tree is not the tagged artifact.
        return git("diff-index", "--quiet", "HEAD", "--") === null ? `${tag}-dirty` : tag;
    }
    return git("describe", "--tags", "--always", "--dirty");
}

function version() {
    const described = describe();
    if (!described) {
        return BASE_VERSION;
    }
    if (/^[0-9a-f]+(-dirty)?$/.test(described)) {
        return `${BASE_VERSION}+g${described}`;
    }
    return described.replace(/^v/, "");
}

console.log(`STABLE_CLICE_VERSION ${version()}`);
