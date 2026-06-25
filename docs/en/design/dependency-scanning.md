# Dependency Scanning

## Background

A C++ language server needs to know the include relationships between files. This requirement comes from several directions:

**Compilation context for headers**: Header files are not listed in the compilation database (CDB), so the language server must select a host source file for each header to obtain compilation commands. This requires knowing "which source files include this header" -- the reverse include relationship. Without this information, users opening a header file cannot get correct diagnostics, completions, or navigation features.

**Module dependency resolution**: Compiling C++20 modules requires knowledge of inter-module dependencies. The mapping from module names to files and the import relationships between modules must be discovered by scanning file contents.

**Priority scheduling**: Background indexing needs to determine the order in which files are indexed. Files that have an include relationship with the user's currently open files should be indexed first. The include graph enables tracking this relevance.

The problem is that precisely determining include relationships requires running the full C preprocessor -- expanding all macros and evaluating all conditional compilation directives. For large C++ projects (tens of thousands of files), running the preprocessor on every file during startup takes minutes, which is unacceptable.

clangd's approach is to build the include graph incrementally during background indexing -- each time a file is compiled, its include relationships are recorded. This means that until background indexing completes (which may take tens of minutes), the include graph is incomplete. Users opening header files during this period may not find the correct host source file, leading to incorrect diagnostics. In the clangd community, users have repeatedly reported that headers display incorrect diagnostics for extended periods after startup and that correct behavior requires waiting for background indexing to finish.

clice solves this problem with a purpose-built fast dependency scanner -- trading precision for results that are good enough, with the ability to scan tens of thousands of files within seconds during startup.

## Design

### Core Idea

The core trade-off of dependency scanning is **exchanging precision for speed**. A full preprocessor can precisely determine which includes are active and which are excluded by conditional compilation, but it has a fundamental performance problem: the same header may produce different expansion results in different source file contexts, so each (source file, header) combination must be preprocessed independently -- results cannot be shared. The fast scanner abandons preprocessing, making scan results independent of inclusion context -- each file needs to be scanned only once, and results are shared across all contexts. The cost is that includes from all conditional branches are treated as active, yielding a **superset of all possible include relationships**. Although this superset is larger than the actual include relationships, it is safe for the primary use cases of dependency scanning:

- When finding host source files, the superset may yield more candidate hosts but will never miss a correct one
- When judging file relevance, the superset may consider some unrelated files as related but will never miss truly related files

### Why It Is So Fast

The fast scanner's speed comes from one fundamental design choice and several key optimizations:

**Fundamental advantage: cross-context result sharing**

This is the root reason the fast scanner can be orders of magnitude faster than full preprocessing. Under full preprocessing, when different source files include the same header, the preceding `#define`s and `#include`s differ, so the preprocessing results may be entirely different. This means each (source file, header) combination must be preprocessed independently -- the workload is O(number of source files x average header depth). For a large project with tens of thousands of source files, each including hundreds of headers on average, this amounts to millions of preprocessing operations.

The fast scanner abandons preprocessing, so scan results are **independent of inclusion context** -- the same header yields the same `#include` list regardless of which source file includes it. This means each header needs to be scanned only once, and results are shared across all source files. The workload drops to O(number of unique files) -- typically just thousands to tens of thousands of files, rather than millions of operations.

**Lightweight lexical scanning**

The scanner uses Clang's dependency directive scanner (scanSourceForDependencyDirectives), which identifies only preprocessor directive lines beginning with `#` without performing macro expansion, evaluating conditional expressions, or expanding includes. Scanning each file takes on the order of microseconds.

The scan results are raw include names (e.g., `"foo.h"` or `<vector>`) that require a subsequent path resolution phase to map them to actual file paths.

For includes inside conditional compilation, the scanner tracks `#if`/`#ifdef`/`#ifndef` nesting depth to tag them: includes encountered at a depth greater than zero are marked as "conditional." While this tag does not know the specific evaluation result of the condition, it provides useful metadata.

**Separate path resolution phase**

Mapping include names to actual file paths is a separate phase. The resolver uses a directory listing cache -- it pre-reads the file listing for each directory in the search paths, then checks file existence via in-memory string sets, avoiding large numbers of stat system calls.

Resolution results for angle-bracket includes (e.g., `<vector>`) can be cached across files -- under the same compilation configuration, the same header name always resolves to the same path. Double-quoted includes depend on the includer's directory and cannot be cached across files, but each file typically has very few double-quoted includes.

**Wavefront BFS discovery**

Rather than processing all files at once, scanning unfolds in waves:

- **Wave 0**: Scan all source files in the compilation database (parallel I/O + lexical scanning)
- **Path resolution**: Map discovered include names to file paths, identifying newly discovered headers
- **Wave 1**: Scan the newly discovered headers, discovering their includes...
- Repeat until no new files are found

A key optimization: while the current wave is performing path resolution (serial), the next wave's files can already begin prefetching and scanning (parallel). This pipeline-style overlap hides most I/O latency.

### DependencyGraph Storage Structure

DependencyGraph stores include relationships between files and supports both forward and reverse queries:

**Forward includes**: Given a file and compilation configuration, returns all files it directly includes. Each edge records whether it is a conditional include (distinguished via bit flags). A file may have different include sets under different compilation configurations (because search paths differ), so the key is a (file, configuration) pair.

**Reverse includes**: Given a file, returns all files that directly include it. This is a reverse index built in bulk after all forward includes have been established. It is used for "finding host source files" -- BFS upward from the target header along reverse edges until a source file with a CDB entry is found.

**Module mapping**: A mapping from module names to module interface unit file paths, used for C++20 module dependency resolution. Only interface units (`export module`) are registered in the mapping. A module name may map to multiple paths (when the same module is discovered in different compilation configurations).

### Handling Conditional Includes

The scanner does not evaluate conditional expressions but uses nesting depth tracking to tag includes as "conditional" or "unconditional":

```cpp
#include <vector>           // unconditional (depth 0)
#ifdef USE_BOOST
#include <boost/any.hpp>    // conditional (depth 1)
#ifdef BOOST_HAS_X
#include <boost/x.hpp>      // conditional (depth 2)
#endif
#endif
#include <string>           // unconditional (depth 0)
```

When querying the union of a file's includes across all configurations, if the same header is unconditional in one configuration and conditional in another, unconditional takes precedence -- reflecting the semantics that "in at least one configuration, it will definitely be included."

### Handling Module Declarations

C++20 module declarations (`export module foo;`, `module foo:bar;`) typically appear at the top of a file, and the fast scanner can identify them directly. However, if a module declaration appears inside a conditional compilation block (`#ifdef`), the fast scanner cannot determine which declaration is in effect.

In this case, the scanner sets a `need_preprocess` flag, triggering a precise fallback -- running the preprocessor only on the file's header (up to the module declaration), evaluating conditional expressions to determine the actual module name. This fallback affects very few files (the vast majority of module declarations are at the top level) and does not impact overall scanning speed.

### Caching and Incremental Updates

Scan results are cached at multiple levels:

- **Scan result cache**: Each file's scan results (include list, module declarations) are cached by path_id and reused within a single scan to avoid redundant reads.
- **Directory listing cache**: File listings for each directory in the search paths are cached in memory, populated via concurrent readdir tasks during the initial scan.
- **Include resolution cache**: Resolution results for angle-bracket includes are cached by (configuration, header name), including negative caches for resolution failures.

These caches are currently valid within a single scan invocation. The cache infrastructure is designed to support cross-scan persistence (warm starts), but this has not yet been enabled in the server integration.

### Collaboration with Host Source File Lookup

One of the core uses of dependency scanning is finding host source files for headers. The process is as follows:

1. Starting from the target header, traverse upward via the reverse include index
2. Find all root files that transitively include the target header (files not included by any other file)
3. Filter for source files that have compilation commands in the CDB as candidate hosts
4. Select the first candidate host with a valid include chain as the default host

The include chain lookup uses BFS to guarantee the shortest path is found. The compilation context system then uses this include chain to synthesize the header's compilation environment.

### Precise Scanning and Background Indexing as Supplements

The fast scan provides approximate results during the startup phase. As the server runs, the background indexing system gradually compiles every translation unit in the project, obtaining precise include relationships during compilation (with full preprocessing, evaluating all macros and conditional compilation). These precise include relationships are recorded in the index data (TUIndex/MergedIndex). Note that background indexing currently does not update the DependencyGraph built during startup — the fast-scan include graph remains in use for host source lookup and file-dependency queries throughout the server's lifetime.

Additionally, the system provides a precise scanning mode (scan_precise) -- running the full Clang preprocessor for specific scenarios that require accurate dependency information, such as lazy dependency resolution during module compilation (via CompileGraph's resolve_fn). Precise scan results are resolved file paths (rather than raw include names), accurately reflecting the actual include relationships under a specific compilation configuration.

Fast scanning and background indexing are complementary: fast scanning guarantees that a usable include graph is available within seconds of startup (so users immediately get header file support), while background indexing gradually supplements precise information over the following minutes. The two are not alternatives -- even after background indexing completes, fast scanning results are still used for initial file discovery and building the initial dependency graph.

## Design Decisions and Trade-offs

**Why a superset instead of precise results?** Precise results require running the preprocessor, which takes minutes for tens of thousands of files. The superset completes in seconds and is safe for host lookup and relevance judgment -- finding a few extra candidate hosts is far better than missing the correct one.

**Why not rely solely on background indexing to build a precise include graph?** clangd relies entirely on background indexing to build include relationships, but background indexing may take tens of minutes to complete. During this period, the header file experience is incomplete -- users open a header file and see incorrect diagnostics that take a long time to self-correct. clice uses fast scanning to guarantee a usable include graph within seconds of startup, with background indexing gradually supplementing precise information afterward. The combination ensures both immediate availability and eventual precision.

**Why unfold in waves instead of scanning all files at once?** The system does not know upfront which headers need scanning -- they are not in the CDB and can only be discovered by resolving source files' includes. Wavefront expansion is the natural pattern for lazy discovery, while pipeline overlap maximizes parallelism.

**Why is the conditional include tag useful?** Although the specific condition result is unknown, distinguishing "will definitely be included" from "might be included" provides guidance for host selection. A source file that includes the target header through an unconditional include chain is a more reliable host candidate than one connected through a conditional include chain.

## Known Limitations

- **Macro-ized includes**: Includes of the form `#include MACRO_NAME` cannot be resolved by the fast scanner -- they require macro expansion to determine the actual header name. This pattern is relatively uncommon in real projects; affected files are correctly handled during precise compilation.
- **Conditional compilation precision**: Includes from all conditional branches are recorded, making the include graph larger than the actual one. This is safe for host lookup (nothing is missed) but may cause relevance judgments to be overly broad.
- **Case sensitivity**: On case-insensitive file systems (macOS, Windows), if the case of an include does not match the file name, resolution may fail.
- **Framework search paths**: macOS Framework directories (`-F`, `-iframework`) are not yet supported; Framework includes of the form `<Foo/Bar.h>` cannot be resolved correctly.
