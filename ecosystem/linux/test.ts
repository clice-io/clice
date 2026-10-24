import { perf, regression } from "@clice/tools/ecosystem";

regression();

perf("startup", {}, async (p) => {
    await p.sample(["kernel/sched/core.c", "fs/ext4/inode.c", "include/linux/sched.h"], {
        wipe: "cache",
    });
    await p.duringIndex("kernel/sched/core.c", { minutes: 25 });
});
