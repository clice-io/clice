## Context

`clice` currently implements folding ranges in `src/Feature/FoldingRange.cpp`. The implementation is primarily an AST visitor with extra handling for conditional compilation and `#pragma region` data from `CompilationUnitRef::directives()`. It already covers many structural folds that clangd does not currently expose, such as namespaces, records, function parameter lists, lambda captures, call argument lists, access-specifier sections, and initializer lists.

The request path is currently split across:

- `src/Feature/FoldingRange.cpp` for collection
- `src/Server/Feature.cpp` for LSP rendering
- `src/Server/Lifecycle.cpp` for capability advertisement
- `include/Protocol/Feature/FoldingRange.h` for protocol types
- `tests/unit/Feature/FoldingRangeTests.cpp` for unit coverage

That split reveals three immediate shortcomings:

- the server always emits `kind = "region"` and always sets `collapsedText`
- folding-specific client capabilities are modeled in protocol types but not consumed by the server
- directive-related tests are mostly placeholders and do not assert important behavior

The comparison target for this branch should be fixed and versioned. At tag `llvmorg-21.1.8`, clangd's folding behavior is centered on `clang-tools-extra/clangd/SemanticSelection.cpp`, with request plumbing in `ClangdServer.cpp` and `ClangdLSPServer.cpp`, protocol types in `Protocol.h` and `Protocol.cpp`, and regression coverage in `test/folding-range.test` plus folding-related unit tests. The branch should vendor that focused source snapshot first so implementation discussions can refer to committed upstream code instead of informal recollection.

Compared with that clangd baseline, the current gap is clear:

- clangd already has behavior that `clice` still lacks:
  - multiline comment folding
  - contiguous `//` comment-group folding
  - `lineFoldingOnly` rendering behavior
  - consistent use of standard public folding kinds
  - a more complete and assertion-backed folding-range test matrix
- `clice` already has behavior that clangd does not:
  - richer AST-structure folding
  - `#pragma region` and some conditional-compilation folding
  - `collapsedText`
- `clice` still has obvious opportunities that are not fully implemented yet:
  - fully closing the last `#if/#elif/#else` branch at `#endif`
  - folding inactive branches
  - folding multiline macro definitions
  - grouping contiguous `#include` / `import` blocks
  - capability-aware `kind` and `collapsedText` rendering

In addition, the current public output exposes many internal categories directly through `FoldingRange.kind`, such as `"namespace"`, `"class"`, and `"functionBody"`. This is more aggressive than clangd, but standard LSP only defines `comment`, `imports`, and `region` as interoperable folding kinds. The design therefore needs to preserve richer internal classification while presenting a compatible external contract.

## Goals / Non-Goals

**Goals:**

- Vendor a focused clangd reference snapshot from `llvmorg-21.1.8` and use it as the explicit comparison baseline for this branch.
- Preserve `clice`'s current advantage in AST-structure folding instead of regressing to clangd's much narrower block-only baseline.
- Fill the high-value baseline gaps that clangd already covers, especially multiline comments and `lineFoldingOnly`.
- Turn preprocessor metadata into a differentiating `clice` capability covering conditional branches, macro definitions, and include/import grouping.
- Make folding-range output respect client capabilities with predictable fallback behavior.
- Lock behavior down with unit and integration tests across AST, comments, preprocessor handling, and protocol negotiation.

**Non-Goals:**

- Import clangd implementation code directly into `clice` production paths or make the build depend on the vendored reference snapshot.
- Achieve byte-for-byte or range-for-range parity with clangd in this change.
- Add fine-grained folding for every C++ syntax detail such as template parameter lists, requires-clauses, or attribute arguments before their value is proven.
- Introduce editor-specific behavior that only exists to satisfy one frontend.
- Add cross-file or index-backed folding behavior.

## Decisions

### 1. Vendor a focused clangd reference snapshot in-tree before implementation work

The branch should first download a small, reviewable snapshot of clangd's folding-related sources from tag `llvmorg-21.1.8` into `third_party/clangd/llvmorg-21.1.8/`. The snapshot should include the implementation, request plumbing, protocol types, and relevant tests that explain folding behavior, rather than the whole LLVM tree.

Why:

- it creates a stable review artifact for this exploration branch
- later implementation commits can point at committed upstream code instead of external URLs
- it keeps the eventual runtime change honest about what is parity work and what is a clice-specific extension

Alternative considered:

- Compare against live GitHub URLs only. Rejected because it leaves the analysis outside the repository and makes later review harder.

### 2. Split the folding-range pipeline into collection, normalization, and rendering

The current implementation mixes "how a range is discovered" with "how it is emitted as LSP". The new design separates this into three layers:

- collection: produce internal `RawFoldingRange` entries from AST, comment scanning, and preprocessor metadata
- normalization: sort, deduplicate, validate, and reconcile nested or overlapping ranges
- rendering: decide line/column boundaries, `kind`, and `collapsedText` based on client capabilities

Why:

- `lineFoldingOnly`, `collapsedText`, and standards-compatible kind downgrading are rendering concerns and should not pollute collection logic
- comments, macros, and include/import groups do not naturally belong inside the AST visitor
- future range limiting or prioritization should also live in normalization/rendering instead of collector code

Alternative considered:

- Keep generating final LSP ranges directly inside the visitor. Rejected because capability negotiation and multi-source collection will keep making the function larger and harder to test.

### 3. Keep rich internal categories, but only promise standard-compatible public kinds

Internally, the implementation may still distinguish namespace, class, function body, macro definition, conditional branch, and similar categories so tests, prioritization, and `collapsedText` selection remain precise. However, public LSP output should default to standard kinds only:

- comment folds -> `comment`
- contiguous include/import groups -> `imports`
- all other structural and preprocessor folds -> `region`

If some client later proves it needs clice-specific kinds, that can be evaluated separately. This change does not make non-standard kind strings part of the compatibility contract.

Why:

- many current custom strings will not be understood by clients and do not produce stable UI semantics
- the real differentiator is what `clice` can fold, not the literal `kind` label
- once public kinds are standardized, `collapsedText` and range boundaries become the primary user-visible expression

Alternative considered:

- Continue exposing all custom kinds directly. Rejected because that leaves client compatibility up to luck rather than protocol design.

### 4. Use the clangd snapshot as a behavior reference, not as a direct implementation template

clangd's folding logic is text- and token-oriented rather than AST-oriented. `clice` should study the upstream behavior to match the useful parts, but it should not force its own collector architecture to look like clangd's when `CompilationUnitRef::directives()` and the existing AST visitor provide better raw data.

Why:

- parity should be measured at the behavior boundary, not by mirroring file structure
- `clice` already has data sources that clangd does not, especially for directive metadata
- this keeps the change focused on correctness and value, not on source-level imitation

Alternative considered:

- Rewrite `clice` folding collection to resemble clangd's text parser closely. Rejected because that would discard existing strengths without a clear benefit.

### 5. Implement comment folding through lexical/source scanning, not AST

Multiline comments are handled independently in clangd's pseudo-parser path, and `clice` should do the same. The design adds a comment collector that scans the main-file source or token stream directly:

- fold multiline `/* ... */` block comments
- fold contiguous `//` comment groups
- do not fold single-line comments
- adjust closing boundaries for `lineFoldingOnly` mode so the final visible line is not swallowed incorrectly

Why:

- comments are not AST structure, so trying to derive them from AST produces fragile behavior
- lexical scanning naturally handles adjacent-comment grouping and block-comment boundaries

Alternative considered:

- Only support block comments. Rejected because clangd already demonstrates that contiguous `//` comment groups are a useful folding case.

### 6. Rework preprocessor folding around complete branch blocks instead of the current half-open stack

Today `collect_condition_directives()` only closes the previous branch when it sees `#else`, but when it sees `#endif` it only pops the stack and does not emit a folding range for the final `#if/#elif/#else` branch. As a result, `#if` folding is incomplete.

The new design treats conditional compilation as an explicit branch-group model:

- maintain the ordered branch chain for each `#if` group
- allow every branch to close at the next `#elif`, `#else`, or `#endif`
- distinguish active and inactive branches
- allow inactive branches to produce region folds, optionally with distinct `collapsedText`

Why:

- this is the minimum sound model needed to fix the current logical gap
- `Condition::ConditionValue` already records true/false/skipped state and can drive inactive-branch folding directly

Alternative considered:

- Patch only the `#endif` closing case. Rejected because nested conditions, inactive branches, and range ordering would remain structurally weak.

### 7. Add dedicated directive-based collectors for macros and include/import groups

`clice` already collects:

- `directive.macros`
- `directive.includes`
- `directive.imports`

The new design therefore adds directive-based folding collectors for:

- multiline `#define` macro definitions, using continuation backslashes or stable definition ranges
- contiguous `#include` blocks, merged into a single `imports` folding range
- contiguous `import Foo;` / `import Foo:Bar;` module-import blocks, also emitted as `imports`

Why:

- the necessary data already exists in preprocessing metadata and does not require new AST modeling
- this is one of the easiest places for `clice` to provide value beyond clangd

Alternative considered:

- Leave include/import grouping for a later change. Rejected because the metadata already exists, the implementation cost is relatively low, and the editor-facing value is immediate.

### 8. Separate clangd parity capabilities from clice-only protocol improvements

This change should treat `lineFoldingOnly` and standard public kinds as clangd parity work, while `collapsedText` gating and deterministic `rangeLimit` trimming are tracked as clice-side protocol improvements. The implementation can ship them together, but the design and tests should not imply that clangd already provides all of them.

Why:

- it keeps the comparison honest
- it allows reviewer discussion to separate "must match upstream baseline" from "valuable extra behavior"
- it keeps spec language compatible with LSP without overstating clangd

Alternative considered:

- Treat all capability work as a clangd parity gap. Rejected because clangd's known folding path does not establish that broader claim.

### 9. Folding-range output must be explicitly bound to client capabilities

The master server currently only advertises `foldingRangeProvider = true`, but it does not read or propagate folding-specific client capabilities. The new design requires the session to track at least:

- `lineFoldingOnly`
- whether `collapsedText` is supported
- optional `rangeLimit`

Rendering rules:

- when `lineFoldingOnly = true`, only emit ranges that remain meaningful as line-based folds, adjusting end lines where necessary
- when the client does not support `collapsedText`, omit it
- when a `rangeLimit` is declared, trim results deterministically rather than arbitrarily

Alternative considered:

- Continue always returning exact columns and `collapsedText`. Rejected because that relies on client tolerance instead of following the protocol contract.

### 10. Organize tests by source category and protocol behavior

Tests will be split into two dimensions:

- source-category unit tests: AST structure, comments, conditional compilation, multiline macros, `#pragma region`, and include/import groups
- protocol-behavior tests: `lineFoldingOnly`, `collapsedText` support, public kind mapping, and range limiting

In particular, the current `tests/unit/Feature/FoldingRangeTests.cpp` contains `Directive` and `PragmaRegion` cases that do not actually assert results. This change upgrades them into strong assertion-based tests.

Alternative considered:

- Rely mostly on manual editor validation. Rejected because folding details regress easily, especially for preprocessor handling and line-only rendering.

## Risks / Trade-offs

- [The vendored clangd snapshot could sprawl or become noisy in review] -> Mitigation: vendor only the small folding-related file set needed for comparison and keep it in a dedicated commit.
- [Client capabilities must flow from initialize state into request-time rendering] -> Mitigation: introduce a dedicated folding-options structure so session details do not leak broadly into the feature layer.
- [Inactive-branch and macro-definition ranges can be unstable around expansion locations] -> Mitigation: prefer spelling/main-file ranges and explicitly filter or special-case macro-expansion ranges when necessary.
- [Adding comments, macros, and include/import groups can increase the number of ranges quickly] -> Mitigation: implement stable sorting and `rangeLimit` trimming in the normalization layer.
- [Mapping public kinds back to standard values changes current metadata output] -> Mitigation: the folds themselves remain; the user-visible change is mostly in optional metadata, and tests plus change notes will make that explicit.
- [Multiple collectors may produce overlapping or duplicate ranges] -> Mitigation: normalize by source category and boundary rules so collectors do not amplify noise.

## Migration Plan

1. Vendor the focused clangd `llvmorg-21.1.8` folding snapshot into `third_party/clangd/llvmorg-21.1.8/` and commit it separately.
2. Record the confirmed clangd-vs-clice comparison in this change, including which behaviors are parity gaps and which are clice-specific extensions.
3. Refactor the internal folding-range data flow and add render options plus standard kind mapping.
4. Add the comment collector and assertion-backed tests for multiline comment folding.
5. Rewrite conditional-directive and `#pragma region` collection so `#if` branches close correctly through `#endif`.
6. Add multiline macro folding and grouped include/import collectors.
7. Wire folding client capabilities through initialize/request handling and add integration coverage.
8. Add `rangeLimit` trimming and regression cleanup after the new collectors are in place.

Rollback strategy:

- If the vendored snapshot becomes more distracting than useful, keep only the documented comparison notes and drop the snapshot commit before merging the implementation branch.
- If protocol negotiation proves unstable, keep the new collectors but temporarily disable outward behavior changes tied to `collapsedText` or `rangeLimit`.
- If a particular new fold category proves noisy, roll it back collector-by-collector instead of reverting the entire folding-range refactor.

## Open Questions

- Which exact clangd unit test files from `llvmorg-21.1.8` are worth vendoring alongside `test/folding-range.test`, and which are just noise for this branch?
- Should multiline macro folding cover only the macro body, or the full `#define NAME(...)` line plus body as one fold region?
- Should `rangeLimit` prioritize outer structure, top-of-file regions, or longer ranges when trimming results?
- For structural AST folds originating from macro expansion, should `clice` preserve current behavior or restrict itself to cases with stable spelling ranges only?
