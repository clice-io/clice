/// The clice executable the tests run: CLICE_EXECUTABLE, with `.exe`
/// supplied on Windows.

import * as fs from "node:fs";
import * as path from "node:path";

export function cliceExecutable(): string {
    let exe = process.env["CLICE_EXECUTABLE"];
    if (!exe) {
        throw new Error("CLICE_EXECUTABLE is not set; point it at build/<type>/bin/bin/clice");
    }
    if (process.platform === "win32" && !exe.toLowerCase().endsWith(".exe")) {
        const withSuffix = `${exe}.exe`;
        if (fs.existsSync(withSuffix) || !fs.existsSync(exe)) {
            exe = withSuffix;
        }
    }
    if (!fs.existsSync(exe)) {
        throw new Error(`clice executable not found at '${exe}'`);
    }
    return path.resolve(exe);
}
