import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { elideCrash } from "../render/lsp.ts";
import { normalize, splitCommand } from "./run.ts";

describe("splitCommand", () => {
    test("quotes group words", () => {
        expect(splitCommand(`query --name "a b" --path='x y.cpp'  --limit 3`)).toEqual([
            "query",
            "--name",
            "a b",
            "--path=x y.cpp",
            "--limit",
            "3",
        ]);
        expect(splitCommand(`format ""`)).toEqual(["format", ""]);
    });

    test("an open quote is refused", () => {
        expect(() => splitCommand(`query --name "a`)).toThrow(/unterminated/);
    });
});

describe("normalize", () => {
    test("host details are elided", () => {
        const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-norm-")));
        try {
            const text = [
                `${root}/src/a.cpp`,
                `${root}/.clice/logs/2026-10-11_02-58-20_88194/master.log`,
                `${root}/logs/2026-10-11_02-58-20_88194`,
                "indexed 9 units in 1.25s, 3ms per unit, 12.5 KB",
                "built at 2026-10-12 04:41:13  generation=1",
            ].join("\n");
            expect(normalize(text, root).split("\n")).toEqual([
                "${WS}/src/a.cpp",
                "${WS}/.clice/logs/${SESSION}/master.log",
                "${WS}/logs/${SESSION}",
                "indexed 9 units in #s, #ms per unit, # KB",
                "built at #time  generation=1",
            ]);
        } finally {
            fs.rmSync(root, { recursive: true });
        }
    });

    test("a worker's death reads the same everywhere", () => {
        expect(elideCrash("Worker SL-0 killed by signal 4 (crash streak: 0)")).toBe(
            "Worker SL-0 ${CRASH} (crash streak: 0)",
        );
        expect(elideCrash("Worker SL-0 exited with code -1073741795 (crash streak: 0)")).toBe(
            "Worker SL-0 ${CRASH} (crash streak: 0)",
        );
    });
});
