/// Workspace — a directory a clice server is pointed at, with relative-path
/// file operations, CDB generation and cache-store inspection as members,
/// so tests never juggle absolute paths or raw fs calls.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { URI } from "vscode-uri";
import { generateCDB } from "../compile_commands.ts";

/// Versioned root of the unified cache store; bump together with
/// cache_format_version in src/server/state/workspace.h.
const CACHE_ROOT = path.join(".clice", "cache", "v7");

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
        const data = entries.map(([f, args]) => ({
            directory: this.root,
            file: this.path(f),
            arguments: [
                "clang++",
                `-std=${options.std ?? "c++17"}`,
                "-fsyntax-only",
                ...args,
                this.path(f),
            ],
        }));
        this.write("compile_commands.json", JSON.stringify(data, null, 2));
    }

    /// Generate compile_commands.json via CMake (workspaces with a
    /// CMakeLists.txt).
    generateCDB(): void {
        generateCDB(this.root);
    }

    /// Write a clice.toml that pins cache_dir to <workspace>/.clice/.
    pinCacheDir(): void {
        this.write("clice.toml", '[project]\ncache_dir = "${workspace}/.clice"\n');
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

    /// Read and parse cache.json, or return null if absent. Plain JSON.parse
    /// mangles the 64-bit dep hashes; tests that rewrite the file use the
    /// lossless helpers in persistent_cache.test.ts.
    readCacheJson(): CacheJson | null {
        const p = path.join(this.cacheRoot(), "cache.json");
        if (!fs.existsSync(p)) {
            return null;
        }
        return JSON.parse(fs.readFileSync(p, "utf8")) as CacheJson;
    }
}

export interface CacheDep {
    path: number;
    /// 64-bit content hash; bigint when read through a lossless parser
    /// (the value overflows a JS double).
    hash: number | bigint;
    mtime_ns?: number;
    size?: number;
    [key: string]: unknown;
}

export interface CacheEntry {
    key?: string;
    deps: CacheDep[];
    bound?: number;
    build_at?: number;
    source_file?: number;
    module_name?: string;
    [key: string]: unknown;
}

export interface CacheJson {
    pch: CacheEntry[];
    pcm?: CacheEntry[];
    paths: string[];
    [key: string]: unknown;
}
