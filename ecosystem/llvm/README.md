# llvm

The largest C++ project most clice users open, and the one clice is built
against. `all` builds eight subprojects (clang, clang-tools-extra, lld, lldb,
MLIR, Polly, Flang, BOLT) with their TableGen outputs, too many units for one
runner: the regression splits it in five parts. `clang` is what someone
working on clang configures, clang alone for X86, the perf jobs' workload:
its full index, restarts on it, edits and requests in a 22,000-line source,
a large header and a header nearly every unit includes, multi-file sessions
with a branch switch, and fresh servers on sampled files. `all` also times
the dependency scan of the whole tree and fresh starts in three subprojects.
