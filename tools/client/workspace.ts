/// Workspace — a directory a clice server is pointed at, with relative-path
/// file operations, CDB generation and cache-store inspection as members,
/// so tests never juggle absolute paths or raw fs calls.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { URI } from "vscode-uri";
import { buildCDBEntry, generateCDB } from "../compile_commands.ts";

/// Versioned root of the unified cache store; bump together with
/// cache_format_version in src/server/state/workspace.h.
const CACHE_ROOT = path.join(".clice", "cache", "v8");

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

export interface CDBOptions {
    extraArgs?: string[] | undefined;
    std?: string | undefined;
}

export class Workspace {
    // Not a constructor parameter property: those don't survive Node's
    // strip-only TS mode, and bench.ts runs this file under plain node.
    readonly root: string;

    constructor(root: string) {
        this.root = root;
    }

    /// A fresh temp-directory workspace. Removal is the creator's business —
    /// the session fixture registers it for teardown.
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

    /// Canonical file:// URI of a workspace-relative path — safe to compare
    /// against client-normalized server URIs and diagnostics-map keys.
    uri(rel = ""): string {
        return canonicalUri(URI.file(this.path(rel)).toString());
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

    mkdir(rel: string): void {
        fs.mkdirSync(this.path(rel), { recursive: true });
    }

    rm(rel: string): void {
        fs.rmSync(this.path(rel), { recursive: true, force: true });
    }

    /// Remove the whole workspace directory.
    remove(): void {
        fs.rmSync(this.root, { recursive: true, force: true });
    }

    /// Write a compile_commands.json for the given source files.
    writeCDB(files: string[], options: CDBOptions = {}): void {
        this.writeEntries(
            files.map((f) => [f, options.extraArgs ?? []]),
            options,
        );
    }

    /// Write a compile_commands.json with per-file extra arguments; a file
    /// may appear multiple times to model multi-configuration projects.
    writeEntries(entries: [string, string[]][], options: CDBOptions = {}): void {
        const data = entries.map(([f, args]) =>
            buildCDBEntry(this.root, this.path(f), {
                extraArgs: args,
                std: options.std,
            }),
        );
        this.write("compile_commands.json", JSON.stringify(data, null, 2));
    }

    /// Generate compile_commands.json via CMake (workspaces with a
    /// CMakeLists.txt).
    generateCDB(): void {
        generateCDB(this.root);
    }

    /// Write a clice.toml that pins cache_dir to <workspace>/.clice/. With
    /// fsIndex, the index database uses the per-file backend so tests can
    /// read (and rewrite) the persisted metadata blobs directly.
    pinCacheDir(opts: { fsIndex?: boolean } = {}): void {
        const lines = ["[project]", 'cache_dir = "${workspace}/.clice"'];
        if (opts.fsIndex) {
            lines.push('index_db = "files"');
        }
        this.write("clice.toml", lines.join("\n") + "\n");
    }

    /// The versioned cache store root.
    cacheRoot(): string {
        return this.path(CACHE_ROOT);
    }

    private globCache(sub: string, suffix: string): string[] {
        const dir = path.join(this.cacheRoot(), sub);
        if (!fs.existsSync(dir)) {
            return [];
        }
        return fs
            .readdirSync(dir)
            .filter((name) => name.endsWith(suffix))
            .sort()
            .map((name) => path.join(dir, name));
    }

    /// All .pch files in the cache store, sorted. A .pch.idx never matches
    /// (it does not end in ".pch").
    pchFiles(): string[] {
        return this.globCache("pch", ".pch");
    }

    pchIdxFiles(): string[] {
        return this.globCache("pch", ".pch.idx");
    }

    pcmFiles(): string[] {
        return this.globCache("pcm", ".pcm");
    }

    /// In-flight tmp files of all store instances. Committed blobs appear
    /// atomically, so anything under tmp/ is either an in-flight write of a
    /// live server or crash residue awaiting cleanup.
    tmpFiles(): string[] {
        const tmpDir = path.join(this.cacheRoot(), "tmp");
        if (!fs.existsSync(tmpDir)) {
            return [];
        }
        return fs
            .readdirSync(tmpDir, { recursive: true, encoding: "utf8" })
            .map((name) => path.join(tmpDir, name))
            .filter((p) => {
                // A live server may commit or remove a blob between the
                // readdir and this stat; a vanished entry is simply not an
                // in-flight file.
                try {
                    return fs.statSync(p).isFile();
                } catch {
                    return false;
                }
            })
            .sort();
    }

    /// The persisted artifact-metadata blob of the per-file index backend
    /// (pin it with pinCacheDir({ fsIndex: true })).
    artifactsBlobPath(): string {
        return path.join(this.cacheRoot(), "index-artifacts", "artifacts.idx");
    }

    /// Read and parse the artifacts blob, or return null if absent. Dep
    /// hashes are 64-bit integers that overflow JS doubles; oversized
    /// values ride as BigInt (reviver source text in) and writeArtifactsBlob
    /// emits them back verbatim (JSON.rawJSON out) — a mangled hash fails
    /// revalidation and forces a spurious rebuild.
    readArtifactsBlob(): ArtifactsBlob | null {
        const p = this.artifactsBlobPath();
        if (!fs.existsSync(p)) {
            return null;
        }
        type Reviver = (key: string, value: unknown, ctx: { source?: string }) => unknown;
        const parse = JSON.parse as unknown as (text: string, reviver: Reviver) => unknown;
        return parse(fs.readFileSync(p, "utf8"), (_key, value, ctx) =>
            typeof value === "number" && !Number.isSafeInteger(value) && ctx.source !== undefined
                ? BigInt(ctx.source)
                : value,
        ) as ArtifactsBlob;
    }

    writeArtifactsBlob(blob: ArtifactsBlob): void {
        const raw = (JSON as unknown as { rawJSON: (text: string) => unknown }).rawJSON;
        fs.writeFileSync(
            this.artifactsBlobPath(),
            JSON.stringify(blob, (_key, value: unknown) =>
                typeof value === "bigint" ? raw(value.toString()) : value,
            ),
        );
    }
}

export interface ArtifactDep {
    path: number;
    /// 64-bit content hash; bigint when the value overflows a JS double.
    hash: number | bigint;
    size: number | bigint;
    mtime_ns: number | bigint;
    missing: boolean;
}

export interface ArtifactPchEntry {
    key: string;
    bound: number;
    deps: ArtifactDep[];
    [key: string]: unknown;
}

export interface ArtifactPcmEntry {
    key: string;
    source_file: number;
    module_name: string;
    deps: ArtifactDep[];
    [key: string]: unknown;
}

export interface ArtifactsBlob {
    paths: string[];
    pch_index_format: number;
    pch: ArtifactPchEntry[];
    pcm: ArtifactPcmEntry[];
    header_modes: unknown[];
    [key: string]: unknown;
}
