import { perf, regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "10-24#9",
            why: "clang's CUDA has no host overload of CUDA's float max, which nvcc gives host code",
            kind: "compile",
            detail: /no matching function for call to 'max'/,
        },
        {
            issue: "10-24#9",
            why: "clang's CUDA refuses an initialized __shared__ variable, which nvcc is told to allow",
            kind: "compile",
            detail: /initialization is not supported for __shared__ variables/,
        },
    ],
});

perf("sessions", {}, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index(files[0] ?? "");
    await p.session(files);
    await p.sample(files);
});
