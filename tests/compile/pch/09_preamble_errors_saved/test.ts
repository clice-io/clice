import { spawn } from "node:child_process";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

const GENERATED = "#pragma once\ninline int generated() {\n    return 1;\n}\n";

async function pchBuilds(s: Serve, file: string): Promise<number | undefined> {
    await s.sync();
    return (await s.counts()).files[file]?.pch;
}

/// Holds `file` open as a virus scanner or an encryption filter can, letting
/// no other program read it, until the function it returns lets it go.
async function hold(file: string): Promise<() => Promise<void>> {
    const holder = spawn(
        "powershell",
        [
            "-NoProfile",
            "-Command",
            `$held = [IO.File]::Open('${file.replaceAll("'", "''")}', 'Open', 'ReadWrite', 'None'); ` +
                "'held'; " +
                "[void][Console]::In.ReadLine(); $held.Close()",
        ],
        { stdio: ["pipe", "pipe", "inherit"] },
    );
    const exited = new Promise<void>((resolve) => {
        holder.once("exit", () => {
            resolve();
        });
    });
    await new Promise<void>((resolve, reject) => {
        holder.stdout.once("data", () => {
            resolve();
        });
        void exited.then(() => {
            reject(new Error(`could not hold ${file}`));
        });
    });
    return async () => {
        holder.stdin.end("\n");
        await exited;
    };
}

// An error inside a header the preamble includes stands until the header
// changes, which the PCH's inputs see: a save builds nothing again.
test("error in a header", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("broken", s.errors("main.cpp"));
    s.edit("main.cpp", { replace: "other() + 0", with: "other() + 1" });
    s.save("main.cpp");
    await s.errors("main.cpp");
    expect(await pchBuilds(s, "main.cpp")).toBe(1);
});

// A framework directory records no place its lookup looked in: the header
// appearing there is found once the file is saved.
test("framework header appears", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("missing", s.errors("framework.cpp"));
    t.disk.write("frameworks/Gen.framework/Headers/gen.h", GENERATED);
    s.save("framework.cpp");
    expect(await s.errors("framework.cpp")).toEqual([]);
    expect(await pchBuilds(s, "framework.cpp")).toBe(2);
});

// The build running when the file is saved looked before the header
// appeared: the save overtakes it, and the build after it finds the header.
test("save during a build", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await s.errors("framework.cpp");
    const build = await s.park("pch", "framework.cpp", { at: "reply" });
    t.disk.edit("other.h", { replace: "return 1;", with: "return 2;" });
    const rebuilt = s.errors("framework.cpp");
    await build.reached();
    t.disk.write("frameworks/Gen.framework/Headers/gen.h", GENERATED);
    s.save("framework.cpp");
    await build.release();
    void rebuilt.catch(() => undefined);
    expect(await s.errors("framework.cpp")).toEqual([]);
});

// A save while the build an earlier save asked for runs leaves that build
// alone: restarting it at every save would keep a file saved every second
// from ever getting its PCH.
test("saves while its build runs", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await s.errors("framework.cpp");
    const build = await s.park("pch", "framework.cpp", { at: "reply" });
    s.save("framework.cpp");
    const saved = s.errors("framework.cpp");
    await build.reached();
    s.save("framework.cpp");
    await build.release();
    void saved.catch(() => undefined);
    await s.errors("framework.cpp");
    expect(await pchBuilds(s, "framework.cpp")).toBe(2);
});

// A header another program held while the PCH built is one the build could
// not open, and the error says why: nothing the build looked at changes
// when the program lets it go, so a save looks again.
test(
    "header held by another program",
    {
        except: {
            linux: "Linux lets every program read a file another holds",
            darwin: "macOS lets every program read a file another holds",
        },
    },
    async (t) => {
        const release = await hold(path.join(t.root, "held.h"));
        let s: Serve;
        try {
            s = await t.start({ lastRule: { background_index: false } });
            await t.show("held", s.errors("held.cpp"));
        } finally {
            await release();
        }
        s.save("held.cpp");
        expect(await s.errors("held.cpp")).toEqual([]);
        expect(await pchBuilds(s, "held.cpp")).toBe(2);
    },
);
