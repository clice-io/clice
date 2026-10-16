import { generate, test } from "@clice/tools/testing";

const POSIX = { except: { win32: "Windows has no SIGTERM or SIGHUP to send" } };

for (const [name, signals] of [
    ["a second SIGTERM still saves", ["SIGTERM", "SIGTERM"]],
    ["SIGHUP saves", ["SIGHUP"]],
] as const) {
    test(name, POSIX, async (t) => {
        t.disk.write(generate.units(6));
        await t.cli("index --jobs 1", { interrupt: { units: 1, signals: [...signals] } });
        await t.cli("index");
    });
}
