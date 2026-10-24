/// Workspace — a directory a clice server is pointed at, with relative-path
/// file operations and its servers' logs as members, so callers never
/// juggle absolute paths or raw fs calls.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { URI } from "vscode-uri";
import { logFiles } from "../process_gate.ts";

/// The harness-wide canonical URI spelling: percent-decoded. vscode-uri
/// encodes the drive colon (file:///c%3A/...) while the server emits it
/// literally (file:///c:/...); both decode to one form. Every URI used
/// as an identity — map keys, expected values — must pass through here.
export function canonicalUri(uri: string): string {
    try {
        return decodeURIComponent(uri);
    } catch {
        return uri;
    }
}

export class Workspace {
    // Not a constructor parameter property: those don't survive Node's
    // strip-only TS mode, and the ecosystem rounds run this file under plain
    // node.
    readonly root: string;

    constructor(root: string) {
        this.root = root;
    }

    /// A fresh temp-directory workspace. Removal is the creator's business.
    ///
    /// realpath matters: macOS's tmpdir is a symlink (/var/folders ->
    /// /private/var), and the server canonicalizes discovered TU paths, so
    /// un-resolved test URIs would miss every closed-TU lookup. pytest's
    /// tmp_path resolved too — this is parity, not a workaround.
    static tmp(): Workspace {
        return new Workspace(
            fs.realpathSync.native(fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"))),
        );
    }

    toString(): string {
        return this.root;
    }

    /// Absolute path of a workspace-relative path (absolute passes through).
    path(rel = ""): string {
        return path.isAbsolute(rel) ? rel : path.join(this.root, rel);
    }

    /// The file:// URI of a workspace-relative path as an editor sends it,
    /// percent-encoded; compared with one the server sent, both go through
    /// canonicalUri.
    uri(rel = ""): string {
        return URI.file(this.path(rel)).toString();
    }

    /// How the server spells a workspace path in text (hover cards, CLI
    /// output): forward slashes and, on Windows, a lowercase drive letter.
    displayPath(rel = ""): string {
        return this.path(rel)
            .replaceAll("\\", "/")
            .replace(/^[A-Za-z]:/, (drive) => drive.toLowerCase());
    }

    exists(rel: string): boolean {
        return fs.existsSync(this.path(rel));
    }

    read(rel: string): string {
        return fs.readFileSync(this.path(rel), "utf8");
    }

    /// Write a file, creating parent directories as needed.
    write(rel: string, content: string): void {
        const target = this.path(rel);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, content);
    }

    rm(rel: string): void {
        fs.rmSync(this.path(rel), { recursive: true, force: true });
    }

    /// Remove the whole workspace directory.
    remove(): void {
        fs.rmSync(this.root, { recursive: true, force: true });
    }

    /// The text of every log file named `name` ("master.log", "SF-0.log")
    /// the servers wrote under .clice/logs, one session directory each;
    /// empty when none was written.
    log(name: string): string {
        return logFiles(this.root)
            .filter((file) => path.basename(file) === name)
            .map((file) => fs.readFileSync(file, "utf8"))
            .join("");
    }

    /// The workers that crashed on requests whose tag starts with `tag`
    /// ("compile /abs/path"), counted from the crash lines the master logs
    /// with the worker's name in front (the crash report repeats them bare).
    workerCrashes(tag: string): number {
        return (
            this.log("master.log").split(`] clice worker crashed in: clice/worker/${tag}`).length -
            1
        );
    }
}
