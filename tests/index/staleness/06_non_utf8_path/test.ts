import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "@clice/tools/testing";

// A source whose real name is not UTF-8, reached through a symlink: an
// edit of its command still reindexes it. The disk actions name files by
// strings, so the name of raw bytes is made through fs.
test(
    "command edit reindexes",
    {
        except: {
            win32: "only Linux file systems take a name that is not UTF-8",
            darwin: "only Linux file systems take a name that is not UTF-8",
        },
    },
    async (t) => {
        const target = Buffer.concat([
            Buffer.from(`${t.root}${path.sep}`),
            Buffer.from([0xff]),
            Buffer.from(".cpp"),
        ]);
        fs.writeFileSync(target, "int f() { return X; }\n");
        fs.symlinkSync(target, path.join(t.root, "a.cpp"));
        t.disk.database({ "a.cpp": ["-DX=1"] });
        await t.cli("index");
        t.disk.database({ "a.cpp": ["-DX=2"] });
        await t.cli("index");
    },
);
