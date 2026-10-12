import { expect } from "vitest";
import { generate, test, type TestContext } from "@clice/tools/testing";

// A command's output goes wherever the shell put it: to a reader that
// leaves (`| head`), to a FIFO, to a descriptor it cannot write. About
// 1 MB of document symbols is more than a pipe and a reader's first read
// hold on every platform.

const SYMBOLS = 6000;
function inspect(file: string): string[] {
    return ["inspect", "document_symbol", file];
}

const INSPECT = inspect("big.cpp");

function big(t: TestContext): void {
    t.disk.write("big.cpp", generate.functions(SYMBOLS));
}

const POSIX = { except: { win32: "Windows has no FIFOs or sh" } };

// A reader leaving ends the command quietly, as SIGPIPE ends any other.
test("a reader that leaves", async (t) => {
    big(t);
    await t.cli(INSPECT, { stdout: "closed early" });
});

// A shell's `| head` is a pipe, where the test's own is a socket.
test("a FIFO whose reader left", POSIX, async (t) => {
    big(t);
    await t.cli(INSPECT, { stdout: "fifo gone" });
});

test(
    "a full nonblocking pipe",
    {
        except: {
            win32: "Windows has no FIFOs or sh",
            darwin: "Node hands a macOS child every descriptor blocking",
        },
    },
    async (t) => {
        big(t);
        await t.cli(INSPECT, { stdout: "fifo unread" });
    },
);

test(
    "an unwritable output",
    { except: { win32: "the error names the system's reason, which Windows words apart" } },
    async (t) => {
        await t.cli(inspect("main.cpp"), { stdout: "read only" });
    },
);

// `2>&1` shares stderr's open file with stdout: only a serving master may
// make it nonblocking, or a slow reader fails the writes.
test("stderr merged into stdout", POSIX, async (t) => {
    big(t);
    const run = await t.cli(INSPECT, { stdout: "merged" });
    const files = (run.json as { files: Record<string, { result: unknown[] }> }).files;
    expect(files["big.cpp"]?.result).toHaveLength(SYMBOLS);
});
