import { generate, test } from "@clice/tools/testing";

// Ctrl-C stops a run where it stands and saves the units it indexed; the
// next run indexes only the rest.

test({ except: { win32: "Windows has no SIGINT to send a process" } }, async (t) => {
    t.disk.write(generate.units(6));
    await t.cli("index --workers 1", { interrupt: { units: 1, signals: ["SIGINT"] } });
    await t.cli("index");
});
