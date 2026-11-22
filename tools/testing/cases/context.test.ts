import { spawn, type ChildProcess } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test, vi } from "vitest";
import type { Case } from "./case.ts";
import { runRegistered } from "./context.ts";

describe("runRegistered", () => {
    test("a failed test is named before its processes and its copy go", async () => {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-src-"));
        const c: Case = {
            id: "topic/subtopic/01_failing",
            topic: "topic",
            subtopic: "subtopic",
            name: "01_failing",
            dir,
            project: {},
            inputs: [],
            markers: [],
            readme: undefined,
            script: "",
        };
        let root = "";
        let child: ChildProcess | undefined;
        const announced: { text: string; running: boolean; copied: boolean }[] = [];
        const write = vi.spyOn(process.stderr, "write").mockImplementation((text) => {
            announced.push({
                text: String(text),
                running: child?.exitCode === null && child.signalCode === null,
                copied: fs.existsSync(root),
            });
            return true;
        });
        try {
            await expect(
                runRegistered(c, {
                    kind: "test",
                    name: "leaves a process",
                    options: {},
                    fn: (t) => {
                        root = t.root;
                        child = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
                            cwd: t.root,
                            stdio: "ignore",
                        });
                        return Promise.reject(new Error("the script failed"));
                    },
                }),
            ).rejects.toThrow("the script failed");
        } finally {
            write.mockRestore();
            fs.rmSync(dir, { recursive: true });
        }
        expect(announced).toEqual([
            {
                text: "topic/subtopic/01_failing > leaves a process failed: the script failed\nsteps:\n\n",
                running: true,
                copied: true,
            },
        ]);
        expect(child?.signalCode).toBe("SIGKILL");
        expect(fs.existsSync(root)).toBe(false);
    });
});
