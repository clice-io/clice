/// File references in answers, checked and made workspace-relative.
///
/// A file URI the server sends is an invariant, not a best-effort input:
/// it must be a well-formed `file://` URI whose decoded path exists, and a
/// raw path or an unencoded space fails here on every platform instead of
/// breaking real clients on Windows. vscode-uri decodes; it never stands in
/// for the checks.

import * as fs from "node:fs";
import * as path from "node:path";
import { URI } from "vscode-uri";

/// Directories outside the workspace answers may point into, and the name
/// their files render under (`${RESOURCE}/include/stddef.h`).
export interface Root {
    dir: string;
    name: string;
}

// Bytes RFC 3986 does not allow raw in a URI (spaces, control characters,
// non-ASCII): their presence means the server skipped percent-encoding.
const URI_ILLEGAL_CHAR = /[^A-Za-z0-9\-._~:/?#[\]@!$&'()*+,;=%]/;
const URI_BAD_PERCENT = /%(?![0-9A-Fa-f]{2})/;

/// The decoded absolute path of a URI as the protocol carries it.
export function uriToPath(uri: string): string {
    const illegal = URI_ILLEGAL_CHAR.exec(uri);
    if (illegal) {
        throw new Error(
            `raw ${JSON.stringify(illegal[0])} in URI (missing percent-encoding): ${uri}`,
        );
    }
    if (URI_BAD_PERCENT.test(uri)) {
        throw new Error(`malformed percent-encoding in URI: ${uri}`);
    }
    // The scheme is checked on the raw text: URI.parse quietly upgrades a
    // bare path to a file URI, which is the malformed reply to catch.
    const scheme = (/^([A-Za-z][A-Za-z0-9+.-]*):/.exec(uri)?.[1] ?? "").toLowerCase();
    if (scheme !== "file") {
        throw new Error(`not a file:// URI (scheme ${JSON.stringify(scheme)}): ${uri}`);
    }
    const parsed = URI.parse(uri);
    if (parsed.authority !== "" || parsed.query !== "" || parsed.fragment !== "") {
        throw new Error(`file URI with an authority, query or fragment: ${uri}`);
    }
    if (!path.isAbsolute(parsed.fsPath)) {
        throw new Error(`URI does not decode to an absolute path: ${uri}`);
    }
    return parsed.fsPath;
}

/// The name a file renders under: workspace-relative POSIX, or under the
/// name of the root holding it.
export function displayPath(file: string, workspace: string, roots: readonly Root[] = []): string {
    if (!fs.existsSync(file)) {
        throw new Error(`answer names a file that does not exist: ${file}`);
    }
    const resolved = fs.realpathSync.native(file);
    for (const root of [{ dir: workspace, name: "" }, ...roots]) {
        const relative = path.relative(fs.realpathSync.native(root.dir), resolved);
        if (!relative.startsWith("..") && !path.isAbsolute(relative)) {
            const posix = relative.split(path.sep).join("/");
            return root.name === "" ? posix : `${root.name}/${posix}`;
        }
    }
    throw new Error(`answer names a file outside the workspace: ${file}`);
}

export function displayUri(uri: string, workspace: string, roots: readonly Root[] = []): string {
    return displayPath(uriToPath(uri), workspace, roots);
}
