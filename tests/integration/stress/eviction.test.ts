/// Worker document eviction: opening more files than a stateful worker's
/// LRU cap must not silently break features on the evicted documents.

import * as fs from "node:fs";
import * as path from "node:path";
import { writeCdb } from "../../tools/compile_commands.ts";
import { expect, test } from "../../tools/fixtures.ts";

const FILE_COUNT = 18; // one stateful worker holds at most 16 compiled documents

test("evicted document recovers", async ({ session }) => {
    const { client, workspace } = session.tmp();
    const names: string[] = [];
    for (let i = 0; i < FILE_COUNT; i++) {
        const name = `file_${String(i).padStart(2, "0")}.cpp`;
        fs.writeFileSync(path.join(workspace, name), `int value_${i} = ${i};\n`);
        names.push(name);
    }
    writeCdb(workspace, names);
    // A single stateful worker so all opens land in one document cache.
    await client.initialize(workspace, {
        initializationOptions: { project: { stateful_worker_count: 1 } },
    });

    const [firstUri] = await client.openAndWait(path.join(workspace, names[0]!));
    for (const name of names.slice(1)) {
        await client.openAndWait(path.join(workspace, name));
    }

    // The first file was LRU-evicted from the worker; hover must trigger a
    // recompile instead of silently returning null against the lost AST.
    const hover = await client.hoverAt(firstUri, 0, 4);
    expect(hover, "hover on the evicted document must recover").not.toBeNull();
});
