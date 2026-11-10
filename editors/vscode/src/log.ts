import * as readline from "readline";
import type * as stream from "stream";
import * as vscode from "vscode";
import type { LogOutputChannel, OutputChannel } from "vscode";
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

function timestamp(): string {
    const now = new Date();
    const pad = (value: number, width = 2) => String(value).padStart(width, "0");
    return (
        `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ` +
        `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}.` +
        pad(now.getMilliseconds(), 3)
    );
}

/// The channel of the LSP trace. vscode-languageclient traces while its
/// trace channel's level is Trace, which this one takes from
/// `clice.trace.server` alone; the channel is created by its first line, so
/// a session that never traces lists none.
export class TraceChannel implements LogOutputChannel {
    readonly name = "clice (LSP trace)";
    private output: OutputChannel | undefined;
    private readonly changed = new vscode.EventEmitter<vscode.LogLevel>();
    private readonly watch = vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration("clice.trace.server")) {
            this.changed.fire(this.logLevel);
        }
    });
    readonly onDidChangeLogLevel = this.changed.event;

    get logLevel(): vscode.LogLevel {
        const setting = vscode.workspace
            .getConfiguration("clice")
            .get<string>("trace.server", "off");
        return setting === "off" ? vscode.LogLevel.Off : vscode.LogLevel.Trace;
    }

    private channel(): OutputChannel {
        this.output ??= vscode.window.createOutputChannel(this.name);
        return this.output;
    }

    private line(level: Level, message: string | Error): void {
        this.channel().appendLine(`[${timestamp()}] [${level}] ${String(message)}`);
    }

    trace(message: string): void {
        this.line("trace", message);
    }

    debug(message: string): void {
        this.line("debug", message);
    }

    info(message: string): void {
        this.line("info", message);
    }

    warn(message: string): void {
        this.line("warning", message);
    }

    error(error: string | Error): void {
        this.line("error", error);
    }

    append(value: string): void {
        this.channel().append(value);
    }

    appendLine(value: string): void {
        this.channel().appendLine(value);
    }

    replace(value: string): void {
        this.channel().replace(value);
    }

    clear(): void {
        this.output?.clear();
    }

    show(column?: vscode.ViewColumn | boolean, preserveFocus?: boolean): void {
        this.channel().show(typeof column === "boolean" ? column : preserveFocus);
    }

    hide(): void {
        this.output?.hide();
    }

    dispose(): void {
        this.watch.dispose();
        this.changed.dispose();
        this.output?.dispose();
    }
}
