import * as readline from "readline";
import type * as stream from "stream";
import type { LogOutputChannel } from "vscode";
import type { StdioOptions } from "vscode-languageclient/node";

type Level = "trace" | "debug" | "info" | "warning" | "error" | "fatal";

/// A line of clice's log: its date, time and `[level]`, then the rest.
const LINE = /^\[[^\]]+\] \[(trace|debug|info|warning|error|fatal)\] (.*)$/;

function write(channel: LogOutputChannel, level: Level, text: string): void {
    switch (level) {
        case "trace":
            channel.trace(text);
            break;
        case "debug":
            channel.debug(text);
            break;
        case "info":
            channel.info(text);
            break;
        case "warning":
            channel.warn(text);
            break;
        default:
            channel.error(text);
    }
}

/// Each line of the server's at its own level in the channel, which writes
/// its own time and level before it; a line of none, a crash's stack trace,
/// continues the line before it.
function forward(input: stream.Readable, channel: LogOutputChannel): void {
    let level: Level = "info";
    readline
        .createInterface({ input, crlfDelay: Infinity, terminal: false, historySize: 0 })
        .on("line", (line) => {
            const parsed = LINE.exec(line);
            if (parsed === null) {
                write(channel, level, line);
                return;
            }
            level = parsed[1] as Level;
            write(channel, level, parsed[2]);
        });
}

export const serverOutput: Required<StdioOptions> = { stdout: forward, stderr: forward };
