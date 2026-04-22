## Downloaded Upstream Reference

Downloaded from GitHub tag `llvmorg-21.1.8` into `openspec/changes/explore-improve-folding-range-support/reference/clangd/llvmorg-21.1.8/` using `curl`.

Files downloaded:

- `clang-tools-extra/clangd/SemanticSelection.cpp`
- `clang-tools-extra/clangd/SemanticSelection.h`
- `clang-tools-extra/clangd/ClangdServer.cpp`
- `clang-tools-extra/clangd/ClangdServer.h`
- `clang-tools-extra/clangd/ClangdLSPServer.cpp`
- `clang-tools-extra/clangd/Protocol.h`
- `clang-tools-extra/clangd/Protocol.cpp`
- `clang-tools-extra/clangd/test/folding-range.test`
- `clang-tools-extra/clangd/unittests/SemanticSelectionTests.cpp`

Raw GitHub URLs used:

- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/SemanticSelection.cpp`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/SemanticSelection.h`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/ClangdServer.cpp`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/ClangdServer.h`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/ClangdLSPServer.cpp`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/Protocol.h`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/Protocol.cpp`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/test/folding-range.test`
- `https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.8/clang-tools-extra/clangd/unittests/SemanticSelectionTests.cpp`

## Clice Reference Files

Current branch files compared against:

- `src/feature/folding_ranges.cpp`
- `src/server/master_server.cpp`
- `tests/unit/feature/folding_range_tests.cpp`

## Confirmed Comparison Findings

### 1. clangd already has dedicated comment folding and line-only rendering

clangd's pseudo-parser folding path in `SemanticSelection.cpp` explicitly handles:

- bracket folds with line-only adjustment at `SemanticSelection.cpp:223-235`
- multiline block and contiguous comment-group folds at `SemanticSelection.cpp:238-269`

The request path wires `LineFoldingOnly` from client capabilities in:

- `ClangdLSPServer.cpp:545`
- `ClangdServer.cpp:967-980`

Regression coverage exists in:

- `test/folding-range.test:6-20`
- `unittests/SemanticSelectionTests.cpp:269-455`

Current clice does not have any comment collector in `src/feature/folding_ranges.cpp`, and the server request path in `src/server/master_server.cpp:517-525` forwards folding requests without any folding-specific options.

### 2. clice already folds more AST structure than clangd

clangd's AST-oriented `getFoldingRanges(ParsedAST &AST)` is intentionally narrow and only walks syntax-tree compound statements in `SemanticSelection.cpp:170-175`.

Current clice already folds:

- namespaces at `src/feature/folding_ranges.cpp:66-80`
- records and access-specifier regions at `src/feature/folding_ranges.cpp:82-121`
- function parameter lists and bodies at `src/feature/folding_ranges.cpp:123-144`, `246-269`
- lambda captures at `src/feature/folding_ranges.cpp:134-143`
- call argument lists at `src/feature/folding_ranges.cpp:146-179`
- initializer lists at `src/feature/folding_ranges.cpp:181-185`
- compound statements at `src/feature/folding_ranges.cpp:271-284`

That is materially broader than clangd's current AST folding baseline.

### 3. clice still exposes richer but less compatible output

Current clice maps many internal categories directly to custom kind strings in `src/feature/folding_ranges.cpp:35-54`, and carries `collapsed_text` through `src/feature/folding_ranges.cpp:56-60` and `src/feature/folding_ranges.cpp:363-376`.

clangd's downloaded protocol reference exposes only folding kinds in `Protocol.h:1970-1981` and serializes them in `Protocol.cpp:1680-1692`. The downloaded clangd protocol does not expose `collapsedText`, so `collapsedText` is a clice-specific protocol improvement rather than a clangd parity requirement.

### 4. clice still has an incomplete `#endif` branch closure bug

Current clice closes a prior conditional branch only when `#else` is seen at `src/feature/folding_ranges.cpp:302-311`. On `#endif`, it only pops the stack at `src/feature/folding_ranges.cpp:314-317` and emits no range for the final branch body.

clangd does not solve this either. Upstream `SemanticSelection.cpp:178-190` still leaves PP conditional regions and disabled regions as FIXME items. This means `#if` branch folding remains a clice extension opportunity, not a direct clangd parity target.

### 5. clice lacks client-capability plumbing for folding

Current clice only advertises `caps.folding_range_provider = true` in `src/server/master_server.cpp:244`, and the request handler in `src/server/master_server.cpp:517-525` forwards no `lineFoldingOnly`, `rangeLimit`, or `collapsedText` support signals into the feature layer.

clangd at least threads `LineFoldingOnly` from the client into folding generation via `ClangdLSPServer.cpp:545` and `ClangdServer.cpp:974-976`.

### 6. clice test coverage is still weaker in the most important gap areas

Current clice has structural tests, but the directive and pragma-region cases remain placeholder-only in `tests/unit/feature/folding_range_tests.cpp:398-430`. The tests also do not assert folding kinds.

clangd's downloaded tests cover:

- AST folding
- comment folding
- line-folding-only behavior
- macro-related exclusion cases

Those are visible in `unittests/SemanticSelectionTests.cpp:269-455`.

## Planning Implications

The downloaded source narrows the real parity target:

- confirmed clangd parity gaps for clice: comment folding, `lineFoldingOnly`, standard public kind behavior, stronger tests
- confirmed clice advantages over clangd: namespaces, access-specifier regions, lambda captures, function parameter folds, function-call folds, initializer folds, pragma regions, collapsed text
- confirmed clice-specific extension space beyond clangd: inactive-branch folding, complete `#if/#elif/#else/#endif` folding, macro-definition folding, include/import grouping

The earlier `third_party` vendor plan was the wrong storage model for this branch. The correct model is a change-local downloaded reference under `openspec/changes/explore-improve-folding-range-support/reference/`.
