# Toolchain Changelog

Pitfalls of building clice with xclang's toolchain and libclang under Bazel
(`MODULE.bazel`, `.bazelrc`, `BUILD.bazel`, `bazel/`, CI) that still apply;
`llvm-changelog.md` covers clang API changes. Every entry: what you see, why,
the fix, and how to check it before spending CI time. Add one with every
toolchain change; drop one once nothing can bring it back.

## What xclang owns

xclang's Bazel module carries the ThinLTO cache (clice's `.bazelrc` only
names the directory, `--repo_env=XCLANG_THINLTO_CACHE`), `-oso_prefix` on
macOS (`--keep-icf-stabs` only on the links that make a dSYM), stripping by
object format, the choice of the ASan libclang (`@libclang` follows
`--features=asan`), the Windows system libraries of `@libclang`,
`clang-tidy-config.h`, the resource directory (`xclang_resource_dir`) and a
program's GSYM and dSYM (`xclang_debug_symbols`). `compile_commands.json`
comes from the `compdb` module of the same registry. Windows builds target
MinGW (`*-w64-mingw32`), the one Windows ABI xclang has. A problem in any of
these is fixed in xclang and arrives with a new release; clice-side
workarounds for them do not belong in clice's Bazel files.

## Bazel and CI

- **A build for macOS on a Linux or Windows host fails: "xclang builds for
  macOS on macOS hosts only".** xclang's macOS toolchains need a macOS host.
  CI cross-builds arm64 Linux and both Windows targets on x64 Linux, x64
  macOS on arm64 macOS (`build_os` in `test.yml`), and tests on the target's
  runner. Check: `npx bazel build --platforms=@xclang//platforms:aarch64-unknown-linux-gnu //:bin/clice`
  on x64 Linux; `file` names aarch64.
- **Every link of clice takes minutes.** The libclang archives hold ThinLTO
  bitcode; lld redoes its code generation on every link unless its cache has
  it. The cache directory is one path per OS (`.bazelrc`: the path is in the
  links' action keys), under `/var/tmp` on Linux, which the sandbox may
  write (`--sandbox_writable_path`); CI keeps it in the Bazel cache entry.
  Check: after a link `/var/tmp/xclang-thinlto` (`C:/xclang-thinlto`) is
  full, and relinking after an edit takes seconds.
- **Two builds of one commit write different `clice.gsym` files.**
  llvm-gsymutil orders its output by thread scheduling. Fix:
  `--num-threads=1` in `xclang_debug_symbols`' `gsymutil_args`
  (`bazel/defs.bzl`). Check: build `//:gsym` twice without the disk cache
  and compare.
- **Fewer Windows unit tests than the last green main run's Windows legs, or
  `clice --help` aborting with "The <name> subcommand is not registered".**
  lld's `--gc-sections` drops a COMDAT nothing references, initializer and
  all, and an archive member nothing references is never linked. zest's
  registrations are ordinary variables since kotatsu 0.1.0.226, the
  driver's too, and the `driver` library is `alwayslink`, so both programs
  link with `gc_sections` on every OS. Check: the Windows legs'
  `N tests from M test suites` line matches the last green main run's
  (Linux runs more: some tests are compiled out on Windows);
  `clice --help` lists every subcommand.
- **Windows: the version is `0.1.0`.** `--workspace_status_command=<script>.sh`
  runs through `cmd.exe`, which hands a `.sh` to its file association and
  reads nothing back. Fix: `bazel/workspace_status.mjs`, run by node on every
  host. Check: `clice --version` on Windows names the commit.
- **macOS: a module unit compiles as if its dependencies' `defines` were
  unset** (spdlog falls back to its bundled fmt, `fmt_lib` names `fmt`).
  rules_cc's macOS feature list left the defines to Bazel's legacy feature,
  whose actions are not the C++20 module compiles and scans; xclang
  23.1.2.8 puts `preprocessor_defines_feature` in the list. Check:
  `npx bazel aquery 'mnemonic("CppCompile", //modules:deps)'` on macOS lists
  `-DSPDLOG_COMPILED_LIB`.

## macOS

- **The dSYM, hence the GSYM, has no line information for half of clice's
  sources** (`crash_handler` symbolizes without `logging.cpp`). A partition
  `foo.cppm` and its implementation unit `foo.cpp` both compile to `foo.o`
  in one archive; the debug map names archive members by name, and dsymutil
  takes the first. xclang 23.1.2.9 has `supports_start_end_lib` on Linux and
  macOS, so Bazel links the objects themselves. Check:
  `llvm-nm -ap bin/clice | grep ' OSO ' | grep -c '\.a('` prints 0.
- **`clice.gsym` names functions but no lines, or the debugger finds no
  debug info.** Mach-O programs keep their DWARF in the object files,
  recorded by absolute path; xclang's `-oso_prefix` makes the paths relative
  to the execution root, and its `generate_dsym_file` feature links the dSYM
  `xclang_debug_symbols` converts. Check: `dsymutil -s build/RelWithDebInfo/bin/bin/clice | grep OSO`
  names `bazel-out/...`; `scripts/symbolize.py` on a macOS crash log prints
  `file:line` for clice's frames.
- **The binary links the SDK's `libc++.dylib`.** `--no-default-config`
  (conda-forge's clang habit) drops xclang's config files, which select its
  static, hermetic libc++ (hidden visibility, libc++abi merged in). Never
  pass it. The SDK's `libc++.1.dylib` still arrives through system frameworks
  (libuv → CoreFoundation → libobjc); two libc++ in one process is expected.
  Check: `scripts/check_artifact_deps.py` (CI runs it on every packaged leg).

## Windows

- **Any compile for `x86_64-pc-windows-msvc` stops at
  `cannot read configuration file`**, also `-ffreestanding` ones and clice's
  driver queries. xclang 23.1.2.8's MSVC config files read
  `../sdk/windows/<target>.cfg`, which only `xclang sdk fetch windows`
  writes; 23.1.2.9's load without it. Check: `clang++ --target=x86_64-pc-windows-msvc -ffreestanding -nostdinc -fsyntax-only`
  on an empty file exits 0 on Linux.
- **MSVC's headers are not found unless `VCToolsInstallDir` is set or
  `cl.exe` is in `PATH`** (#714); the include paths name Visual Studio
  2010–2005. LLVM builds its Visual Studio Setup API lookup (`MSVCPaths.cpp`)
  only with MSVC, and xclang's Windows libclang is MinGW-built. xclang's
  `patches/0004` builds it on MinGW until llvm/llvm-project#226794 lands.
  Check: xclang's Windows smoke test, `clang++ --target=<arch>-pc-windows-msvc -###`
  naming `VC\Tools\MSVC\` with no Visual Studio environment.
- **An `llvm.exe clang++ ...` process left suspended forever** (0 CPU, one
  suspended thread, parent gone), holding its working directory, so a case
  fails deleting its workspace (`EPERM`) and the runner never exits; seen
  when a test kills the server during a toolchain query. xclang's launcher
  (every `clang++.exe` and other `llvm.exe` name) used to start `llvm.exe`
  suspended and assign it to its kill-on-close job afterwards; since 23.1.2.4
  it creates the child inside the job (`PROC_THREAD_ATTRIBUTE_JOB_LIST`).
  Check: kill a launcher right after it starts a long-running `llvm.exe`
  (`Stop-Process -Force`); the child must be gone.
- **`pixi run` hangs for minutes on the Windows checkout.** A `pixi install`
  in progress holds the environment lock: wait or stop it
  (`tasklist | findstr pixi`).

## libclang and the driver

- **The toolchain query finds no clang job in the driver's `-v` output.**
  xclang's tools are one multi-call `llvm` binary: the job reads
  `"…/bin/llvm" "clang" "-cc1" …`, and `parse_cc1` skips the tool name
  before `-cc1`. Check: `ToolchainTests.ParseCC1MultiCall`.
- **A `g++` command, queried through the in-process driver, gets libc++'s
  headers** (no `<print>`). xclang 23.1.2.1 built clang with
  `CLANG_DEFAULT_CXX_STDLIB=libc++` (and the runtime and linker defaults),
  and libclang's driver shares them; since 23.1.2.2 they are upstream's and
  xclang's config files choose its runtimes. Check: `clang++ --no-default-config -### x.o`
  of the xclang in use links `-lstdc++`.
- **The master or a worker crashes on a log line with a string of 256 bytes,
  or a multiple of it** (a file path of exactly 256 bytes). libc++'s
  `format_to` into a container writes past its 256-code-unit stack buffer
  after such an argument (llvm/llvm-project#154670), and spdlog's
  `std::format` back end goes through it. xclang's `patches/0006` until
  llvm/llvm-project#226791 lands. Check: xclang's smoke test formats 256 `a`
  and a literal into a string under ASan.
- **Completion or signature help on a constructor call inside a class
  template crashes a worker** (#701): a clang 19 regression in the partial
  ordering of the class's constructor templates when its pack follows
  another parameter (llvm/llvm-project#226788). xclang's `patches/0001`.
  Check: `ConstructorTemplatesOfClassTemplate` in
  `unittests/server/stateless_worker_tests.cpp`.
- **Any CDB entry with `-fsanitize=address` (or another sanitizer with a
  default ignorelist) kills the worker**:
  `LLVM ERROR: can't open file .../lib/clang/<major>/share/asan_ignorelist.txt`,
  signal 6. The driver emits
  `-fsanitize-system-ignorelist=<its resource dir>/share/...` after checking
  the file exists in its own tree, and cc1 opens ignorelists with
  `createOrDie`; clice's resource directory ships `include/` alone. clice
  rewrites only the resource dir itself and `include/` under it; every other
  path the driver derived from its resource dir stays external
  (`src/command/toolchain.cpp`). Check: `ResolveKeepsExternalIgnorelist`, the
  case `tests/compile/flags/03_sanitizers`.
- **A clang assertion aborts a Debug run** (`Qualifier must be dependent`,
  say) where RelWithDebInfo passes. Only the ASan libclang has assertions on:
  the abort is a clice bug the release build swallows. Fix clice; never turn
  the assertions off.
- **lldb says `member access into incomplete type 'clang::TypedefNameDecl'`
  in a clice frame.** `-fno-standalone-debug` (the default) emits a class's
  definition next to its vtable, inside the libclang archives, which carry
  line tables only. Stack traces still resolve; for an interactive session
  build with `--copt=-fstandalone-debug`. Check:
  `expr decl->getNameAsString()` in a clice frame.
