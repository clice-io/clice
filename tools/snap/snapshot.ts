/// Snapshot assertions for the TS test suites, mirroring zest's semantics.
///
/// File format and update workflow follow the C++ side (kota::zest
/// snapshot support) so both layers share one muscle memory:
///
///     ---
///     created_at: 2026-07-26
///     input_file: fold_kinds/block_folding.cpp
///     ---
///     <body>
///
/// A missing snapshot is created and passes; a mismatch writes a sibling
/// `.snap.yml.new` and fails with a diff; UPDATE_SNAPSHOTS=1 rewrites the
/// stored snapshot preserving its created_at.

import * as fs from "node:fs";
import * as path from "node:path";
import { diffLines } from "diff";
import { URI } from "vscode-uri";

export const WORKSPACE_PLACEHOLDER = "${WS}";

// Bytes RFC 3986 does not allow to appear raw in a URI (spaces, control
// characters, non-ASCII, ...). Their presence means the server skipped
// percent-encoding; lenient parsers would silently tolerate them.
const URI_ILLEGAL_CHAR = /[^A-Za-z0-9\-._~:/?#[\]@!$&'()*+,;=%]/;
const URI_BAD_PERCENT = /%(?![0-9A-Fa-f]{2})/;

/// Validate a wire URI and rewrite it to a platform-independent form.
///
/// This is deliberately an invariant check, not a best-effort cleanup: any
/// server response field holding a file reference must be a well-formed
/// `file://` URI whose decoded path exists inside the test workspace.
/// A raw filesystem path fails here on every platform, instead of only
/// breaking real clients on Windows. vscode-uri is used strictly as the
/// decoder; its tolerance for malformed input must never replace the
/// checks above it.
export function normalizeFileUri(uri: string, workspace: string): string {
    const illegal = URI_ILLEGAL_CHAR.exec(uri);
    if (illegal) {
        throw new Error(
            `raw ${JSON.stringify(illegal[0])} in URI (missing percent-encoding): ${uri}`,
        );
    }
    if (URI_BAD_PERCENT.test(uri)) {
        throw new Error(`malformed percent-encoding in URI: ${uri}`);
    }

    // Check the scheme on the raw string: URI.parse in non-strict mode
    // silently upgrades a bare path to a file URI, which is exactly the
    // malformed reply this validator exists to reject.
    // Schemes are case-insensitive (RFC 3986); lowercase like urlsplit did.
    const scheme = (/^([A-Za-z][A-Za-z0-9+.-]*):/.exec(uri)?.[1] ?? "").toLowerCase();
    if (scheme !== "file") {
        throw new Error(`not a file:// URI (scheme=${JSON.stringify(scheme)}): ${uri}`);
    }
    const parsed = URI.parse(uri);
    if (parsed.authority) {
        throw new Error(`unexpected authority in file URI: ${uri}`);
    }
    if (parsed.query || parsed.fragment) {
        throw new Error(`unexpected query/fragment in file URI: ${uri}`);
    }

    // vscode-uri owns the percent-decoding and drive-letter handling.
    const fsPath = parsed.fsPath;
    if (!path.isAbsolute(fsPath)) {
        throw new Error(`URI does not decode to an absolute path: ${uri}`);
    }
    if (!fs.existsSync(fsPath)) {
        throw new Error(`URI target does not exist on disk: ${uri} -> ${fsPath}`);
    }

    const resolved = fs.realpathSync.native(fsPath);
    const ws = fs.realpathSync.native(workspace);
    const rel = path.relative(ws, resolved);
    if (rel.startsWith("..") || path.isAbsolute(rel)) {
        throw new Error(`URI target escapes the workspace ${ws}: ${uri}`);
    }
    return `${WORKSPACE_PLACEHOLDER}/${rel.split(path.sep).join("/")}`;
}

/// Validate a raw filesystem path from a feature payload and rewrite it
/// to the platform-independent `${WS}/...` form — the path-domain sibling
/// of normalizeFileUri, for payloads the reply edge has not yet converted
/// to URIs (e.g. raw document links).
export function normalizeFilePath(target: string, workspace: string): string {
    if (!path.isAbsolute(target)) {
        throw new Error(`link target is not an absolute path: ${target}`);
    }
    if (!fs.existsSync(target)) {
        throw new Error(`link target does not exist on disk: ${target}`);
    }
    const resolved = fs.realpathSync.native(target);
    const ws = fs.realpathSync.native(workspace);
    const rel = path.relative(ws, resolved);
    if (rel.startsWith("..") || path.isAbsolute(rel)) {
        throw new Error(`link target escapes the workspace ${ws}: ${target}`);
    }
    return `${WORKSPACE_PLACEHOLDER}/${rel.split(path.sep).join("/")}`;
}

/// Quote a string exactly like tests/unit/test/tester.h yaml_str, so
/// snapshot bodies read identically across the layers.
export function yamlStr(s: string): string {
    let out = '"';
    for (const c of s) {
        if (c === '"') {
            out += '\\"';
        } else if (c === "\\") {
            out += "\\\\";
        } else if (c === "\n") {
            out += "\\n";
        } else if (c === "\r") {
            out += "\\r";
        } else if (c === "\t") {
            out += "\\t";
        } else if (c.charCodeAt(0) < 0x20) {
            out += `\\x${c.charCodeAt(0).toString(16).padStart(2, "0")}`;
        } else {
            out += c;
        }
    }
    return out + '"';
}

export interface Snapshot {
    createdAt: string;
    body: string;
}

export function parseSnap(text: string): Snapshot | null {
    const lines = text.replaceAll("\r\n", "\n").split("\n");
    if (lines[0] !== "---") {
        return null;
    }
    let createdAt = "";
    for (let i = 1; i < lines.length; i++) {
        const line = lines[i] ?? "";
        if (line === "---") {
            return { createdAt, body: lines.slice(i + 1).join("\n") };
        }
        if (line.startsWith("created_at:")) {
            createdAt = line.slice("created_at:".length).trim();
        }
    }
    return null;
}

export function formatSnap(inputFile: string, body: string, createdAt = ""): string {
    if (!createdAt) {
        createdAt = new Date().toISOString().slice(0, 10);
    }
    if (body && !body.endsWith("\n")) {
        body += "\n";
    }
    return `---\ncreated_at: ${createdAt}\ninput_file: ${inputFile}\n---\n${body}`;
}

function renderDiff(oldBody: string, newBody: string): string {
    // jsdiff owns the diffing; render changed lines only, capped so a
    // wholesale mismatch stays readable.
    const out: string[] = [];
    for (const part of diffLines(oldBody, newBody)) {
        if (!part.added && !part.removed) {
            continue;
        }
        const prefix = part.added ? "+  " : "-  ";
        const lines = part.value.split("\n");
        // Newline-terminated hunks split with a trailing empty element;
        // dropping only that keeps genuinely blank changed lines visible.
        if (lines[lines.length - 1] === "") {
            lines.pop();
        }
        for (const line of lines) {
            out.push(prefix + line);
            if (out.length >= 40) {
                return out.join("\n");
            }
        }
    }
    return out.join("\n");
}

/// One feature's snapshot directory plus the run-wide update flag.
///
/// Snapshots replace the source extension and sit next to the input, with
/// an optional variant infix: `<input>.inspect.snap.yml` /
/// `<input>.server.snap.yml` for `snap: separate` fixtures.
export class SnapshotContext {
    readonly directory: string;
    readonly update: boolean;
    /// `update: false` marks a caller that must never author the file (the
    /// server driver on a shared snapshot) — for it a missing snapshot is
    /// an error, not something to create from its own output.
    readonly create: boolean;

    // Plain field assignments: parameter properties are not erasable
    // syntax, and tools/ scripts run under bare `node` in strip-only mode.
    constructor(directory: string, options: { update?: boolean } = {}) {
        this.directory = directory;
        this.update = options.update ?? process.env["UPDATE_SNAPSHOTS"] === "1";
        this.create = options.update !== false;
    }

    snapPath(inputFile: string, variant = ""): string {
        const suffix = (variant ? `.${variant}` : "") + ".snap.yml";
        const name = inputFile.replace(/\.(cpp|cc|cxx)$/, "") + suffix;
        return path.join(this.directory, name);
    }

    check(inputFile: string, body: string, variant = ""): void {
        body = body.replaceAll("\r\n", "\n");
        if (body && !body.endsWith("\n")) {
            body += "\n";
        }

        const snapPath = this.snapPath(inputFile, variant);
        const newPath = `${snapPath}.new`;

        const existing = fs.existsSync(snapPath)
            ? parseSnap(fs.readFileSync(snapPath, "utf8"))
            : null;
        if (existing === null) {
            if (!this.create) {
                throw new Error(`missing snapshot ${snapPath} (owned by the other path)`);
            }
            fs.mkdirSync(path.dirname(snapPath), { recursive: true });
            fs.writeFileSync(snapPath, formatSnap(inputFile, body));
            console.log(`[snapshot] created ${snapPath}`);
            return;
        }

        if (existing.body === body) {
            fs.rmSync(newPath, { force: true });
            return;
        }

        if (this.update) {
            fs.writeFileSync(snapPath, formatSnap(inputFile, body, existing.createdAt));
            fs.rmSync(newPath, { force: true });
            console.log(`[snapshot] updated ${snapPath}`);
            return;
        }

        fs.writeFileSync(newPath, formatSnap(inputFile, body));
        throw new Error(
            `snapshot mismatch: ${snapPath}\n` +
                `new result written to ${newPath}\n` +
                `${renderDiff(existing.body, body)}\n` +
                "run with UPDATE_SNAPSHOTS=1 to accept",
        );
    }
}
