/// On-disk workspace helpers: cache-store inspection.

import * as fs from "node:fs";
import * as path from "node:path";

/// Versioned root of the unified cache store; bump together with
/// cache_format_version in src/server/state/workspace.h.
const CACHE_ROOT = path.join(".clice", "cache", "v4");

/// Return the versioned cache store root for a workspace.
export function cacheRoot(workspace: string): string {
    return path.join(workspace, CACHE_ROOT);
}

/// Write a clice.toml that pins cache_dir to <workspace>/.clice/.
export function pinCacheToWorkspace(workspace: string): void {
    fs.writeFileSync(
        path.join(workspace, "clice.toml"),
        '[project]\ncache_dir = "${workspace}/.clice"\n',
    );
}

function globFiles(dir: string, suffix: string): string[] {
    if (!fs.existsSync(dir)) {
        return [];
    }
    return fs
        .readdirSync(dir)
        .filter((name) => name.endsWith(suffix))
        .sort()
        .map((name) => path.join(dir, name));
}

/// Return all .pch files in the cache directory, sorted. A .pch.idx never
/// matches (it does not end in ".pch"), mirroring the Python glob.
export function listPchFiles(workspace: string): string[] {
    return globFiles(path.join(cacheRoot(workspace), "pch"), ".pch");
}

/// Return all .pch.idx files in the cache directory, sorted.
export function listPchIdxFiles(workspace: string): string[] {
    return globFiles(path.join(cacheRoot(workspace), "pch"), ".pch.idx");
}

/// Return all .pcm files in the cache directory, sorted.
export function listPcmFiles(workspace: string): string[] {
    return globFiles(path.join(cacheRoot(workspace), "pcm"), ".pcm");
}

/// Return in-flight tmp files of all store instances.
///
/// Committed blobs appear atomically, so anything under tmp/ is either an
/// in-flight write of a live server or crash residue awaiting cleanup.
export function listTmpFiles(workspace: string): string[] {
    const tmpDir = path.join(cacheRoot(workspace), "tmp");
    if (!fs.existsSync(tmpDir)) {
        return [];
    }
    return fs
        .readdirSync(tmpDir, { recursive: true, encoding: "utf8" })
        .map((name) => path.join(tmpDir, name))
        .filter((p) => fs.statSync(p).isFile())
        .sort();
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

/// Read and parse cache.json, or return null if absent.
export function readCacheJson(workspace: string): CacheJson | null {
    const p = path.join(cacheRoot(workspace), "cache.json");
    if (!fs.existsSync(p)) {
        return null;
    }
    return JSON.parse(fs.readFileSync(p, "utf8")) as CacheJson;
}
