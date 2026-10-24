/// The servers a perf job compares, each as an editor starts it: clice
/// under test (and a baseline clice), and the official clangd release the
/// framework pins; where each keeps its cache, and what it holds on disk.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import type * as proto from "vscode-languageserver-protocol";
import { unpackArchive } from "../prepare.ts";
import type { ServerName } from "./results.ts";

/// The clangd release of the LLVM major xclang tracks, as vscode-clangd
/// downloads it.
const CLANGD_VERSION = "23.1.0";
const CLANGD: Partial<Record<NodeJS.Platform, { os: string; sha256: string }>> = {
    linux: {
        os: "linux",
        sha256: "e53b1a96196095faedb7642cf64964f7fb9ad4a0c1f00dd2c172a3d9dcbafdfd",
    },
    darwin: {
        os: "mac",
        sha256: "1082e6638223b785ca2daf0939f13afcd0bb95c84ee9a4bbaff4745365159253",
    },
    win32: {
        os: "windows",
        sha256: "23412a240756a162e7b98a282f36aa2a23a88db5ce16a0cbc4fef7253768c810",
    },
};

/// The pinned clangd, downloaded into `dir` once.
export async function pinnedClangd(dir: string): Promise<string> {
    const release = CLANGD[process.platform];
    if (release === undefined) {
        throw new Error(`clangd publishes no release for ${process.platform}`);
    }
    const target = path.join(dir, `clangd-${CLANGD_VERSION}`);
    await unpackArchive(
        `https://github.com/clangd/clangd/releases/download/${CLANGD_VERSION}/clangd-${release.os}-${CLANGD_VERSION}.zip`,
        release.sha256,
        target,
    );
    return path.join(target, "bin", process.platform === "win32" ? "clangd.exe" : "clangd");
}

export function isClice(server: ServerName): boolean {
    return server !== "clangd";
}

/// What an editor announces: VS Code's capabilities for the features the
/// scenarios ask, work done progress included (clangd reports its
/// background index only to a client that takes it) and the file watching
/// a branch switch needs.
export const CAPABILITIES: proto.ClientCapabilities = {
    window: { workDoneProgress: true },
    workspace: {
        workspaceEdit: { documentChanges: true },
        symbol: {},
        didChangeWatchedFiles: { dynamicRegistration: true },
    },
    textDocument: {
        synchronization: { didSave: true },
        hover: { contentFormat: ["markdown", "plaintext"] },
        completion: {
            completionItem: { snippetSupport: true, labelDetailsSupport: true },
            contextSupport: true,
        },
        signatureHelp: {
            signatureInformation: {
                parameterInformation: { labelOffsetSupport: true },
                activeParameterSupport: true,
            },
            contextSupport: true,
        },
        definition: { linkSupport: true },
        references: {},
        rename: { prepareSupport: true },
        publishDiagnostics: { relatedInformation: true },
        documentSymbol: { hierarchicalDocumentSymbolSupport: true },
        foldingRange: { lineFoldingOnly: true },
        semanticTokens: {
            requests: { full: { delta: true } },
            tokenTypes: [],
            tokenModifiers: [],
            formats: ["relative"],
        },
        inlayHint: {},
        callHierarchy: {},
    },
};

/// The servers' background-index work done progress tokens.
export function indexToken(server: ServerName): string {
    return isClice(server) ? "clice/backgroundIndex" : "backgroundIndexProgress";
}

/// The directories each server keeps its cache in: clice everything in
/// the workspace's .clice (pinned by the client's initialize); clangd its
/// index under .cache/clangd next to the workspace root and the database's
/// directory. The rest of a real project's .cache belongs to other tools.
function cacheDirs(workspace: string, database: string, server: ServerName): string[] {
    return isClice(server)
        ? [path.join(workspace, ".clice")]
        : [...new Set([workspace, database])].map((dir) => path.join(dir, ".cache", "clangd"));
}

/// The server's whole cache gone, or for clice its PCHs alone.
export function wipeCache(
    workspace: string,
    database: string,
    server: ServerName,
    only?: "pch",
): void {
    if (only === "pch") {
        const cache = path.join(workspace, ".clice", "cache");
        for (const version of fs.existsSync(cache) ? fs.readdirSync(cache) : []) {
            fs.rmSync(path.join(cache, version, "pch"), { recursive: true, force: true });
        }
        return;
    }
    for (const dir of cacheDirs(workspace, database, server)) {
        // clice's logs are where its perf lines are read from.
        for (const name of fs.existsSync(dir) ? fs.readdirSync(dir) : []) {
            if (!(isClice(server) && name === "logs")) {
                fs.rmSync(path.join(dir, name), { recursive: true, force: true });
            }
        }
    }
}

/// The bytes `file` occupies on disk. Windows extends clice's LMDB file to
/// its whole reserved map, sparse: only the ranges fsutil lists as
/// allocated count there. A file the server moves meanwhile counts none.
function diskBytes(file: string): number {
    const size = fs.statSync(file, { throwIfNoEntry: false })?.size ?? 0;
    if (process.platform !== "win32" || size < 64 * 1024 * 1024) {
        return size;
    }
    try {
        const ranges = execFileSync("fsutil", ["sparse", "queryrange", file], { encoding: "utf8" });
        return [...ranges.matchAll(/Length: 0x([0-9a-f]+)/gi)].reduce(
            (sum, match) => sum + Number.parseInt(match[1] ?? "0", 16),
            0,
        );
    } catch {
        // Not a sparse file: all of it is on disk.
        return size;
    }
}

/// Bytes the server's cache holds on disk, clice's logs aside: all of it,
/// and its index alone. clangd keeps nothing but the index there (its
/// preambles live in memory or in temporary files); clice keeps its index
/// beside the PCHs and PCMs of its store, under cache/<version>/index.
export function cacheBytes(
    workspace: string,
    database: string,
    server: ServerName,
): { disk: number; index: number } {
    const logs = path.join(workspace, ".clice", "logs");
    let disk = 0;
    let index = 0;
    for (const dir of cacheDirs(workspace, database, server)) {
        if (!fs.existsSync(dir)) {
            continue;
        }
        for (const entry of fs.readdirSync(dir, { recursive: true, withFileTypes: true })) {
            const file = path.join(entry.parentPath, entry.name);
            if (!entry.isFile() || file.startsWith(logs + path.sep)) {
                continue;
            }
            const bytes = diskBytes(file);
            disk += bytes;
            const parts = path.relative(dir, file).split(path.sep);
            if (!isClice(server) || (parts[0] === "cache" && parts[2] === "index")) {
                index += bytes;
            }
        }
    }
    return { disk, index };
}

export function cacheMB(
    workspace: string,
    database: string,
    server: ServerName,
): { diskMB: number; indexMB: number } {
    const { disk, index } = cacheBytes(workspace, database, server);
    const mb = (bytes: number): number => Math.round(bytes / 1024 / 1024);
    return { diskMB: mb(disk), indexMB: mb(index) };
}

/// Drop the OS file cache, so that a cold run reads the project, the
/// system headers and the server itself from disk. Windows has no command
/// for it: its runs stay on a warm file cache.
export function dropFileCache(): boolean {
    switch (process.platform) {
        case "linux":
            execFileSync("sudo", ["-n", "sh", "-c", "sync && echo 3 > /proc/sys/vm/drop_caches"]);
            return true;
        case "darwin":
            execFileSync("sync");
            execFileSync("sudo", ["-n", "purge"]);
            return true;
        default:
            return false;
    }
}
