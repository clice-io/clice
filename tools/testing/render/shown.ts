/// How `show` renders what a request answers: a client registers the
/// rendering, and the request's kind, on the promise its request returns,
/// so a test passes the request itself, `show("hover", s.hover(at))`, and
/// every client renders one answer one way.

import { portable } from "../cli/run.ts";
import { mapText, type Table, type Value } from "./value.ts";

export interface RenderOptions {
    /// The optional parts of the answer to render (a completion's insert
    /// text, a symbol's detail): only those the test is about.
    parts: ReadonlySet<string>;
    /// Every item of a list a test shows only the first of: what two
    /// clients must agree on beyond what shows.
    full: boolean;
}

export type Render<T> = (value: T, options: RenderOptions) => Value;

interface Rendering {
    /// The request, as the block names its answer: `hover`, `highlights`.
    kind: string;
    render: Render<never>;
}

const RENDERINGS = new WeakMap<Promise<unknown>, Rendering>();

export function shown<T>(reply: Promise<T>, kind: string, render: Render<T>): Promise<T> {
    RENDERINGS.set(reply, { kind, render });
    return reply;
}

/// The label a block's known-wrong reason takes, which no answer may use.
export const WRONG = "wrong";

/// What a test shows: what a request answers, passed as the request
/// itself; text, lines or a number as they are; or a record of these.
export type Shown =
    | Promise<unknown>
    | string
    | number
    | readonly string[]
    | { readonly [label: string]: Shown };

export interface ShowOptions {
    /// The optional parts of the answers to show.
    parts?: readonly string[];
}

function renderingOf(value: Promise<unknown>, where: string): Rendering {
    const rendering = RENDERINGS.get(value);
    if (rendering === undefined) {
        throw new Error(
            `show("${where}"): the promise is no request of a client; pass s.hover(...) itself, or text`,
        );
    }
    return rendering;
}

/// What `value` shows as: a request its answer, text and numbers as they
/// are, a record a table of these.
async function answer(value: Shown, options: RenderOptions, where: string): Promise<Value> {
    if (value instanceof Promise) {
        const { render } = renderingOf(value, where);
        return (render as Render<unknown>)(await value, options);
    }
    if (typeof value === "string" || typeof value === "number" || isLines(value)) {
        return value;
    }
    observe(value);
    const out: Record<string, Value> = {};
    for (const [label, entry] of Object.entries(value)) {
        if (label === WRONG) {
            throw new Error(`show("${where}"): '${WRONG}' is the known-wrong reason's key`);
        }
        out[label] = await answer(entry, options, label);
    }
    return out;
}

/// A block of the output: a request's answer under its kind, a record's
/// entries under their labels, anything else under `value`. The copy at
/// `root` reads `${WS}`, a worker's death `${CRASH}`.
export async function block(
    value: Shown,
    options: RenderOptions,
    root: string,
    where: string,
): Promise<Table> {
    let table: Table;
    if (value instanceof Promise) {
        table = { [renderingOf(value, where).kind]: await answer(value, options, where) };
    } else if (typeof value === "object" && !isLines(value)) {
        table = (await answer(value, options, where)) as Table;
    } else {
        table = { value: await answer(value, options, where) };
    }
    return mapText(table, (text) => portable(text, root)) as Table;
}

/// Every request of a record is under way already: one failing behind an
/// earlier failure is handled, not left unobserved. The entries still
/// render in order, each after the one before has finished.
function observe(value: Shown): void {
    if (value instanceof Promise) {
        value.catch(() => undefined);
    } else if (typeof value === "object" && !isLines(value)) {
        Object.values(value).forEach(observe);
    }
}

function isLines(value: readonly string[] | object): value is readonly string[] {
    return Array.isArray(value);
}
