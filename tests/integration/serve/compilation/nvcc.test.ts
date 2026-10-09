/// Integration tests for nvcc compilation databases: the toolchain layer
/// rewrites the nvcc command, probes the toolkit via `nvcc --dryrun`, and
/// parses CUDA files in the device view by default. Windows hosts drive cl,
/// which the query does not support yet.

import { describe } from "vitest";
import type { Serve } from "@clice/tools/actions";
import { asLocations, runProcess } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

const hasNvcc = (await runProcess("nvcc", ["--version"])).status === 0;
const runsNvcc = hasNvcc && process.platform !== "win32";

/// Start over on the nvcc command of `file`: the fixture's database spells
/// every unit with clang.
function nvccDatabase(s: Serve, file: string, command: (path: string) => string): Promise<void> {
    return s.offline(() => {
        const path = s.workspace.path(file);
        s.disk.write(
            "compile_commands.json",
            JSON.stringify([{ directory: s.workspace.root, file: path, command: command(path) }]),
        );
    });
}

describe.skipIf(!runsNvcc)("nvcc", () => {
    serve.files(
        {
            "kernels.cuh":
                "#pragma once\n" +
                "__device__ inline float scale(float* p) { return p[threadIdx.x]; }\n" +
                "#if defined(__CUDA_ARCH__)\n" +
                "inline int device_world = 1;\n" +
                "#else\n" +
                "inline int host_world = 1;\n" +
                "#endif\n",
        },
        { manifest: { units: {} } },
    )("nvcc direct cuh entry", async ({ s }) => {
        await nvccDatabase(s, "kernels.cuh", (path) => `nvcc -c ${path} -o kernels.o`);
        await s.clean("kernels.cuh");

        // The device view applies to the header exactly as it would to a .cu.
        expect(await s.client.inactiveLines(s.uri("kernels.cuh"))).toEqual([5]);
    });

    serve.files(
        {
            "main.cu":
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
        },
        { manifest: { units: {} } },
    )("nvcc cuda device view", async ({ s }) => {
        await nvccDatabase(
            s,
            "main.cu",
            (path) =>
                "nvcc -forward-unknown-to-host-compiler " +
                '"--generate-code=arch=compute_75,code=[compute_75,sm_75]" ' +
                `-x cu -c ${path} -o main.cu.o`,
        );
        await s.clean("main.cu");

        // The launch site resolves into the kernel definition.
        const locs = asLocations(await s.definition(at("main.cu", "kern<<<")));
        expect(locs.length).toBeGreaterThan(0);
        expect(locs.some((loc) => loc.range.start.line === 0)).toBe(true);

        // Device view: the host-side #else branch is the inactive one.
        expect(await s.client.inactiveLines(s.uri("main.cu"))).toEqual([4]);
    });
});
