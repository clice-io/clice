/// How `show` renders what a request answers: a client registers the
/// rendering on the promise its request returns, so a test passes the
/// request itself, `show("hover", s.hover(at))`, and every client renders
/// one answer one way.

import { portable } from "../cli/run.ts";
import { formatBlock } from "../cases/output.ts";

export interface RenderOptions {
    /// The optional parts of the answer to render (a completion's insert
    /// text, a symbol's detail): only those the test is about.
    parts: ReadonlySet<string>;
    /// Every item of a list a test shows only the first of: what two
    /// clients must agree on beyond what shows.
    full: boolean;
}

export type Render<T> = (value: T, options: RenderOptions) => string[];

const RENDERINGS = new WeakMap<Promise<unknown>, Render<never>>();

export function shown<T>(reply: Promise<T>, render: Render<T>): Promise<T> {
    RENDERINGS.set(reply, render);
    return reply;
}

/// The rendering of `value`, a request of a client, or undefined for any
/// other promise.
function renderingOf(value: Promise<unknown>): Render<unknown> | undefined {
    return RENDERINGS.get(value) as Render<unknown> | undefined;
}

/// What a test shows: what a request answers, passed as the request
/// itself; text, lines or a number as they are; or a record of these, one
/// block each.
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

/// The lines `value` shows as, the copy at `root` reading `${WS}` and a
/// worker's death `${CRASH}`.
export async function render(
    value: Shown,
    options: RenderOptions,
    root: string,
    where: string,
): Promise<string[]> {
    let lines: readonly string[];
    if (value instanceof Promise) {
        const rendering = renderingOf(value);
        if (rendering === undefined) {
            throw new Error(
                `show("${where}"): the promise is no request of a client; pass s.hover(...) itself, or text`,
            );
        }
        lines = rendering(await value, options);
    } else if (typeof value === "string") {
        lines = value.split("\n");
    } else if (typeof value === "number") {
        lines = [String(value)];
    } else if (isLines(value)) {
        lines = value;
    } else {
        // Every entry is a request already under way: all are awaited, so
        // none fails unobserved behind an earlier failure.
        const settled = await Promise.allSettled(
            Object.entries(value).map(async ([label, entry]) =>
                formatBlock(label, await render(entry, options, root, label)),
            ),
        );
        return settled.flatMap((result) => {
            if (result.status === "rejected") {
                throw result.reason;
            }
            return result.value;
        });
    }
    return lines.map((line) => portable(line, root));
}

function isLines(value: readonly string[] | object): value is readonly string[] {
    return Array.isArray(value);
}
