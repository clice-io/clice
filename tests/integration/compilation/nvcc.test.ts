/// Integration tests for nvcc compilation databases: the toolchain layer
/// rewrites the nvcc command, probes the toolkit via `nvcc --dryrun`, and
/// parses CUDA files in the device view by default. Windows hosts drive cl,
/// which the query does not support yet.

import { spawnSync } from "node:child_process";
import type { Range } from "vscode-languageserver-protocol";
import { asLocations, sleep, type CliceClient } from "@clice/tools/client";
import { InactiveRegionsNotification, type InactiveRegionsParams } from "@clice/tools/protocol";
import { expect, test } from "../fixtures.ts";

const hasNvcc = spawnSync("nvcc", ["--version"], { stdio: "ignore" }).status === 0;
const runsNvcc = hasNvcc && process.platform !== "win32";

async function waitRegions(captured: InactiveRegionsParams[], timeout = 15_000): Promise<Range[]> {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        const last = captured[captured.length - 1];
        if (last && last.regions.length > 0) {
            return last.regions;
        }
        await sleep(50);
    }
    const last = captured[captured.length - 1];
    return last ? last.regions : [];
}

function capture(client: CliceClient): InactiveRegionsParams[] {
    const captured: InactiveRegionsParams[] = [];
    client.onNotification(InactiveRegionsNotification, (params) => {
        captured.push(params);
    });
    return captured;
}

test.skipIf(!runsNvcc)("nvcc direct cuh entry", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "kernels.cuh",
        "#pragma once\n" +
            "__device__ inline float scale(float* p) { return p[threadIdx.x]; }\n" +
            "#if defined(__CUDA_ARCH__)\n" +
            "inline int device_world = 1;\n" +
            "#else\n" +
            "inline int host_world = 1;\n" +
            "#endif\n",
    );
    workspace.write(
        "compile_commands.json",
        JSON.stringify([
            {
                directory: workspace.root,
                file: workspace.path("kernels.cuh"),
                command: `nvcc -c ${workspace.path("kernels.cuh")} -o kernels.o`,
            },
        ]),
    );
    const captured = capture(client);

    await client.initialize(workspace);
    const [uri] = await client.openAndWait("kernels.cuh");
    client.assertCleanCompile(uri);

    // The device view applies to the header exactly as it would to a .cu.
    const regions = await waitRegions(captured);
    expect(regions.map((r) => [r.start.line, r.end.line])).toEqual([[5, 6]]);
});

test.skipIf(!runsNvcc)("nvcc cuda device view", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cu",
        "__global__ void kern(float* p) { p[threadIdx.x] = 1.0f; }\n" +
            "#if defined(__CUDA_ARCH__)\n" +
            "int device_world = 1;\n" +
            "#else\n" +
            "int host_world = 1;\n" +
            "#endif\n" +
            "int main() {\n" +
            "    float* d = nullptr;\n" +
            "    cudaMalloc(&d, 16);\n" +
            "    kern<<<1, 1>>>(d);\n" +
            "    return 0;\n" +
            "}\n",
    );
    workspace.write(
        "compile_commands.json",
        JSON.stringify([
            {
                directory: workspace.root,
                file: workspace.path("main.cu"),
                command:
                    "nvcc -forward-unknown-to-host-compiler " +
                    '"--generate-code=arch=compute_75,code=[compute_75,sm_75]" ' +
                    `-x cu -c ${workspace.path("main.cu")} -o main.cu.o`,
            },
        ]),
    );
    const captured = capture(client);

    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cu");
    client.assertCleanCompile(uri);

    // The launch site resolves into the kernel definition.
    const locs = asLocations(await client.definitionAt(uri, 9, 4));
    expect(locs.length).toBeGreaterThan(0);
    expect(locs.some((loc) => loc.range.start.line === 0)).toBe(true);

    // Device view: the host-side #else branch is the inactive one.
    const regions = await waitRegions(captured);
    expect(regions.map((r) => [r.start.line, r.end.line])).toEqual([[4, 5]]);
});
