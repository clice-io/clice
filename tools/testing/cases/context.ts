/// What a scenario is handed, `t`, and how one registered test runs and
/// leaves its output.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { Workspace } from "../../client/workspace.ts";
import { withTimeout } from "../../promise.ts";
import { runCommand, splitCommand, type CommandOptions, type CommandRun } from "../cli/run.ts";
import { replay } from "../server/replay.ts";
import { render, type Shown } from "../render/shown.ts";
import { Serve, gateLogs, type ServerOptions } from "../server/serve.ts";
import { SCRIPT_DEADLINE_MS, StepRecord, throwFailures } from "../server/steps.ts";
import { markerOf, runAnswers } from "./answers.ts";
import { at, type MarkerAnchor, type TextAnchor } from "./anchors.ts";
import type { Case } from "./case.ts";
import { Disk } from "./disk.ts";
import { Output } from "./output.ts";
import { materialize } from "./project.ts";
import type { Registered, TestOptions } from "./script.ts";

export interface TestContext {
    /// The copy's root.
    readonly root: string;
    /// Start a server on the copy; every one is shut down and gated at the
    /// end. Its restarts take the same options.
    start(options?: ServerOptions): Promise<Serve>;
    /// Run a command in the copy; its transcript joins the output.
    cli(command: string | readonly string[], options?: CommandOptions): Promise<CommandRun>;
    /// Play the case's trace.jsonl, an editor's recorded session, to a new
    /// server.
    replay(): Promise<void>;
    /// A block of the output: what a request of a server answers, passed as
    /// the request itself, `t.show("hover", s.hover(at))`; text, lines or a
    /// number as they are; or a record of these, one block each. The copy
    /// reads `${WS}`, a worker's death `${CRASH}`.
    show(label: string, value: Shown): Promise<void>;
    readonly disk: Disk;
    at(file: string, anchor: string): TextAnchor;
    /// A marker of the case's sources, valid while its file reads as the
    /// case wrote it.
    marker(name: string): MarkerAnchor;
    /// Let `ms` pass, where time is what the test checks; `why` names the product's
    /// interval it waits out.
    elapse(ms: number, why: string): Promise<void>;
}

class TestRun implements TestContext {
    readonly root: string;
    readonly output = new Output();
    readonly disk: Disk;
    private readonly case: Case;
    private readonly workspace: Workspace;
    private readonly steps = new StepRecord();
    private readonly errors: RegExp[];
    private readonly serves: Serve[] = [];

    constructor(c: Case, options: TestOptions) {
        this.case = c;
        this.root = fs.realpathSync.native(fs.mkdtempSync(path.join(os.tmpdir(), "clice-case-")));
        this.workspace = new Workspace(this.root);
        this.disk = new Disk(this.root, c.project, this.steps);
        this.errors = (options.errors ?? []).map((source) => new RegExp(source));
        materialize(c.dir, this.root);
    }

    /// The project's databases, unless the test wrote one, before the first
    /// server or command sees the copy.
    private prepare(): void {
        if (!this.disk.written) {
            this.disk.database();
        }
    }

    private serve(options: ServerOptions): Serve {
        this.prepare();
        const s = new Serve(this.workspace, options, {
            marks: this.case.markers,
            steps: this.steps,
            disk: this.disk,
        });
        this.serves.push(s);
        return s;
    }

    async start(options: ServerOptions = {}): Promise<Serve> {
        const s = this.serve(options);
        await s.start();
        return s;
    }

    async cli(
        command: string | readonly string[],
        options: CommandOptions = {},
    ): Promise<CommandRun> {
        this.prepare();
        const args = typeof command === "string" ? splitCommand(command) : command;
        const run = await this.steps.run(`clice ${args.join(" ")}`, () =>
            runCommand(this.root, args, options),
        );
        this.output.transcript(run.header, run.body);
        return run;
    }

    async replay(): Promise<void> {
        const s = this.serve({});
        await s.spawn();
        await replay(s, path.join(this.case.dir, "trace.jsonl"));
    }

    async show(label: string, value: Shown): Promise<void> {
        this.output.block(
            label,
            await render(value, { parts: new Set(), full: false }, this.root, label),
        );
    }

    at(file: string, anchor: string): TextAnchor {
        return at(file, anchor);
    }

    marker(name: string): MarkerAnchor {
        return markerOf(this.case, name);
    }

    elapse(ms: number, why: string): Promise<void> {
        return this.steps.run(`let ${String(ms)} ms pass: ${why}`, async () => {
            await new Promise((resolve) => setTimeout(resolve, ms));
        });
    }

    /// Shut down and gate every server; returns what failed.
    async finish(failed: boolean): Promise<unknown[]> {
        if (failed) {
            console.log(`steps:\n${this.steps.render()}`);
        }
        const errors: unknown[] = [];
        for (const s of this.serves) {
            errors.push(...(await s.finish(failed)));
        }
        errors.push(
            ...gateLogs(
                this.workspace,
                { errors: this.errors, servers: this.serves.map((s) => s.faults()) },
                failed,
            ),
        );
        if (process.env["CLICE_KEEP_WORKSPACE"] === undefined) {
            this.workspace.remove();
        } else {
            console.log(`workspace kept at ${this.root}`);
        }
        return errors;
    }
}

/// Run one registered test of a case; returns its output lines.
export async function runRegistered(c: Case, registered: Registered): Promise<string[]> {
    if (registered.kind === "answers") {
        return runAnswers(c, registered.options, registered.fn);
    }
    const run = new TestRun(c, registered.options);
    let failure: { error: unknown } | undefined;
    try {
        await withTimeout(registered.fn(run), SCRIPT_DEADLINE_MS, "the test hung");
    } catch (error) {
        failure = { error };
    }
    throwFailures(failure, await run.finish(failure !== undefined));
    return run.output.lines;
}
