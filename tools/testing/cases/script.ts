/// What a case's test.ts registers: `answers` tests, run on `clice inspect`
/// and a server and compared, and `test` scenarios, run once. A script is
/// imported once and its registrations serve every case that inherits it.

import { pathToFileURL } from "node:url";
import type { AnswersContext } from "./answers.ts";
import type { TestContext } from "./context.ts";

/// Platforms a test has no meaning on, each with the reason: elsewhere it
/// runs, there it is listed as not applicable.
export type Except = Partial<Record<NodeJS.Platform, string>>;

export interface TestOptions {
    except?: Except;
    /// Error-level log lines the test provokes, as regular expressions.
    errors?: string[];
}

export interface AnswersOptions {
    except?: Except;
    /// One client alone, where the other cannot answer, or both pinned
    /// apart, where they answer differently on purpose; the value says why.
    paths?: { inspect: string } | { server: string } | { separate: string };
    /// Options shaped as clice.toml: the server takes them all, inspect the
    /// table of each feature it runs.
    config?: Record<string, unknown>;
    /// Background indexing on the server.
    indexing?: boolean;
    /// The server's client offers UTF-8 positions too, and the server must
    /// take them; it answers in UTF-16 otherwise.
    positionEncoding?: "utf-8";
}

export type Registered =
    | { kind: "test"; name: string; options: TestOptions; fn: (t: TestContext) => Promise<void> }
    | {
          kind: "answers";
          name: string;
          options: AnswersOptions;
          fn: (a: AnswersContext) => Promise<void>;
      };

let sink: Registered[] | null = null;

function register(entry: Registered): void {
    if (sink === null) {
        throw new Error("test() and answers() belong at the top level of a case's test.ts");
    }
    if (sink.some((other) => other.name === entry.name)) {
        throw new Error(`two tests named '${entry.name}'`);
    }
    sink.push(entry);
}

function split(args: unknown[]): { name: string; options: object; fn: unknown } {
    const name = typeof args[0] === "string" ? (args.shift() as string) : "";
    const options = typeof args[0] === "object" ? (args.shift() as object) : {};
    return { name, options, fn: args[0] };
}

interface Register<O, C> {
    (fn: (context: C) => Promise<void>): void;
    (nameOrOptions: string | O, fn: (context: C) => Promise<void>): void;
    (name: string, options: O, fn: (context: C) => Promise<void>): void;
}

/// A scenario: anything on servers, commands and the disk, run once.
export const test: Register<TestOptions, TestContext> = (...args: unknown[]) => {
    const { name, options, fn } = split(args);
    register({ kind: "test", name, options, fn: fn as (t: TestContext) => Promise<void> });
};

/// What clients answer about the code as the case wrote it, on both
/// clients, which must show the same.
export const answers: Register<AnswersOptions, AnswersContext> = (...args: unknown[]) => {
    const { name, options, fn } = split(args);
    register({ kind: "answers", name, options, fn: fn as (a: AnswersContext) => Promise<void> });
};

const loaded = new Map<string, Registered[]>();

/// The tests `file` registers.
export async function loadScript(file: string): Promise<Registered[]> {
    const cached = loaded.get(file);
    if (cached !== undefined) {
        return cached;
    }
    sink = [];
    try {
        await import(pathToFileURL(file).href);
        const registered = sink;
        if (registered.length === 0) {
            throw new Error(`${file} registers no test`);
        }
        if (registered.length > 1 && registered.some((entry) => entry.name === "")) {
            throw new Error(`${file}: an unnamed test must be the only one`);
        }
        loaded.set(file, registered);
        return registered;
    } finally {
        sink = null;
    }
}
