/// What a scenario is handed, `t`, and how one registered test runs and
/// leaves its output.

import * as path from "node:path";
import { running } from "../../client/process.ts";
import { Workspace } from "../../client/workspace.ts";
import { withTimeout } from "../../promise.ts";
import {
    runCommand,
    splitCommand,
    undeclaredDeath,
    type CommandOptions,
    type CommandRun,
} from "../cli/run.ts";
import { block, type ShowOptions, type Shown } from "../render/shown.ts";
import { gateLogs, keepLogs, type Faults, type Started } from "../logs/logs.ts";
import { Serve, type ServerOptions } from "../server/serve.ts";
import { SCRIPT_DEADLINE_MS, StepRecord, throwFailures } from "../server/steps.ts";
import { markerOf, runAnswers } from "./answers.ts";
import { at, locate, type Anchor, type MarkerAnchor, type TextAnchor } from "./anchors.ts";
import type { Case } from "./case.ts";
import { Disk } from "./disk.ts";
import { Output, type Blocks } from "./output.ts";
import { copyRoot, materialize } from "./project.ts";
import type { Registered, TestOptions } from "./script.ts";

export interface TestContext {
    /// The copy's root.
    readonly root: string;
    /// Start a server on the copy; every one is shut down and gated at the
    /// end. Its restarts take the same options.
    start(options?: ServerOptions): Promise<Serve>;
    /// Start a server that waits for the client to speak first: the test
    /// sends the handshake, `s.initialize()` or its own through `s.client`.
    spawn(options?: ServerOptions): Promise<Serve>;
    /// Run a command in the copy; its transcript joins the output.
    cli(command: string | readonly string[], options?: CommandOptions): Promise<CommandRun>;
    /// A block of the output: what a request of a server answers, passed as
    /// the request itself, `t.show("hover", s.hover(at))`; text, lines or a
    /// number as they are; or a record of these, one block each. The copy
    /// reads `${WS}`, a worker's death `${CRASH}`.
    show(label: string, value: Shown, options?: ShowOptions): Promise<void>;
    readonly disk: Disk;
    at(file: string, anchor: string): TextAnchor;
    /// A marker of the case's sources, valid while its file reads as the
    /// case wrote it.
    marker(name: string): MarkerAnchor;
    /// The markers of the case's sources, in path and source order.
    markers(): readonly MarkerAnchor[];
    /// `path:line:column` of an anchor in the copy as it reads now, the
    /// way the commands take a place.
    place(anchor: Anchor): string;
    /// Let `ms` pass, where time is what the test checks; `why` names the product's
    /// interval it waits out.
    elapse(ms: number, why: string): Promise<void>;
    /// Resolves once no process has one of `pids`.
    gone(pids: readonly number[]): Promise<void>;
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
    /// The commands the test ran, and how those that died unasked ended.
    private readonly commands: Started[] = [];
    private readonly deaths: string[] = [];
    /// The crashes the test's commands provoke on purpose.
    private readonly commandFaults: Faults[] = [];
    /// The kills the test's commands were told to cause so far.
    private kills = 0;

    constructor(c: Case, options: TestOptions) {
        this.case = c;
        this.root = copyRoot("clice-case-");
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

    async spawn(options: ServerOptions = {}): Promise<Serve> {
        const s = this.serve(options);
        await s.spawn();
        return s;
    }

    async cli(
        command: string | readonly string[],
        options: CommandOptions = {},
    ): Promise<CommandRun> {
        this.prepare();
        const args = typeof command === "string" ? splitCommand(command) : command;
        const { crash, kill } = options;
        if (crash !== undefined) {
            this.commandFaults.push({ crash });
        }
        const killFile = path.join(".clice", `kill-request-cli-${String((this.kills += 1))}`);
        if (kill !== undefined) {
            this.workspace.write(
                killFile,
                `${kill.request} ${this.workspace.displayPath(kill.file)}`,
            );
        }
        const env =
            crash === undefined && kill === undefined
                ? options.env
                : {
                      ...options.env,
                      CLICE_ANOMALY_NO_TRAP: "1",
                      ...(crash === undefined
                          ? {}
                          : {
                                CLICE_TEST_CRASH_REQUEST: `${crash.request} ${this.workspace.displayPath(crash.file)}`,
                            }),
                      ...(kill === undefined
                          ? {}
                          : { CLICE_TEST_KILL_REQUEST: this.workspace.path(killFile) }),
                  };
        const run = await this.steps.run(`clice ${args.join(" ")}`, () =>
            runCommand(this.root, args, { ...options, ...(env === undefined ? {} : { env }) }),
        );
        if (kill !== undefined) {
            this.commandFaults.push({
                kill: { ...kill, taken: this.workspace.exists(`${killFile}.taken`) },
            });
        }
        this.commands.push({ label: run.label, pid: run.pid, stderr: run.stderr });
        const death = undeclaredDeath(run, options);
        if (death !== undefined) {
            this.deaths.push(death);
        }
        this.output.transcript(run.label, run.transcript);
        return run;
    }

    async show(label: string, value: Shown, options: ShowOptions = {}): Promise<void> {
        this.output.block(
            label,
            await block(value, { parts: new Set(options.parts), full: false }, this.root, label),
        );
    }

    at(file: string, anchor: string): TextAnchor {
        return at(file, anchor);
    }

    marker(name: string): MarkerAnchor {
        return markerOf(this.case, name);
    }

    markers(): readonly MarkerAnchor[] {
        return this.case.markers;
    }

    place(anchor: Anchor): string {
        const text = this.disk.read(anchor.path);
        const lines = text.slice(0, locate(text, anchor)).split("\n");
        return `${anchor.path}:${String(lines.length)}:${String((lines.at(-1) ?? "").length + 1)}`;
    }

    elapse(ms: number, why: string): Promise<void> {
        return this.steps.run(`let ${String(ms)} ms pass: ${why}`, async () => {
            // Time is the feature: a test whose claim is about time.
            await new Promise((resolve) => setTimeout(resolve, ms));
        });
    }

    gone(pids: readonly number[]): Promise<void> {
        return this.steps.run(`processes ${pids.join(", ")} gone`, async () => {
            const deadline = Date.now() + 60_000;
            while (pids.some(running)) {
                if (Date.now() > deadline) {
                    throw new Error(
                        `still running after a minute: ${pids.filter(running).join(", ")}`,
                    );
                }
                // No event: the end of a process not the test's own child is
                // announced to no one; a minute bounds the looking.
                await new Promise((resolve) => setTimeout(resolve, 20));
            }
        });
    }

    /// Shut down and gate every server; returns what failed.
    async finish(failed: boolean): Promise<unknown[]> {
        if (failed) {
            console.log(`steps:\n${this.steps.render()}`);
        }
        const errors: unknown[] = this.deaths.map((death) => new Error(death));
        for (const s of this.serves) {
            errors.push(...(await s.finish(failed)));
        }
        errors.push(
            ...gateLogs(
                this.root,
                [...this.serves.flatMap((s) => s.started()), ...this.commands],
                {
                    errors: this.errors,
                    servers: this.serves.map((s) => s.faults()),
                    commands: this.commandFaults,
                },
                failed,
            ),
        );
        keepLogs(this.case.id, this.root);
        if (process.env["CLICE_KEEP_WORKSPACE"] === undefined) {
            this.workspace.remove();
        } else {
            console.log(`workspace kept at ${this.root}`);
        }
        return errors;
    }
}

/// Run one registered test of a case; returns its blocks.
export async function runRegistered(c: Case, registered: Registered): Promise<Blocks> {
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
    return run.output.blocks;
}
