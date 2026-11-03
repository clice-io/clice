import { perf, regression } from "@clice/tools/ecosystem";

regression();

perf("sessions", {}, async (p) => {
    const files = await p.pick({ sources: 10, headers: 5 });
    await p.index("src/main.cpp");
    await p.session(files);
    await p.sample(files);
});
