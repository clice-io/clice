/// The actions a test ran, a combined action's own nested a level below
/// it, printed when the test fails. A step's depth follows the async
/// context it runs in, so actions awaited side by side nest under their own
/// callers.

import { AsyncLocalStorage } from "node:async_hooks";

interface Step {
    depth: number;
    text: string;
    started: number;
    ended?: number;
    failed?: boolean;
}

export class StepRecord {
    private readonly steps: Step[] = [];
    private readonly depth = new AsyncLocalStorage<number>();

    run<T>(text: string, body: () => Promise<T>): Promise<T> {
        const depth = this.depth.getStore() ?? 0;
        const step: Step = { depth, text, started: Date.now() };
        this.steps.push(step);
        return this.depth.run(depth + 1, async () => {
            try {
                return await body();
            } catch (error) {
                step.failed = true;
                throw error;
            } finally {
                step.ended = Date.now();
            }
        });
    }

    /// A step that waits for nothing.
    note(text: string): void {
        const now = Date.now();
        this.steps.push({ depth: this.depth.getStore() ?? 0, text, started: now, ended: now });
    }

    render(): string {
        const now = Date.now();
        const width = String(this.steps.length).length;
        return this.steps
            .map((step, index) => {
                const head = `step ${String(index + 1).padStart(width)}/${this.steps.length}  ${"  ".repeat(step.depth)}${step.text}`;
                if (step.ended === undefined) {
                    return `${head} ... no answer after ${Math.round((now - step.started) / 1000)} s`;
                }
                return step.failed === true ? `${head} ... failed` : head;
            })
            .join("\n");
    }
}
