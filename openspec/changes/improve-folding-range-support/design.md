## Context

`clice` currently implements folding ranges in [`src/feature/folding_ranges.cpp`](/home/ykiko/C++/clice/src/feature/folding_ranges.cpp). The implementation is primarily an AST visitor with extra handling for conditional compilation and `#pragma region` data from `CompilationUnitRef::directives()`. It already covers many structural folds that clangd does not currently expose, such as namespaces, records, function parameter lists, lambda captures, call argument lists, access-specifier sections, and initializer lists.

Compared with `.llvm/clang-tools-extra/clangd`, the current gap is clear:

- clangd already has behavior that `clice` still lacks:
  - multiline comment folding
  - `lineFoldingOnly` client-capability handling
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

- Preserve `clice`'s current advantage in AST-structure folding instead of regressing to clangd's much narrower block-only baseline.
- Fill the high-value baseline gaps that clangd already covers, especially multiline comments and `lineFoldingOnly`.
- Turn preprocessor metadata into a differentiating `clice` capability covering conditional branches, macro definitions, and include/import grouping.
- Make folding-range output respect client capabilities with predictable fallback behavior.
- Lock behavior down with unit and integration tests across AST, comments, preprocessor handling, and protocol negotiation.

**Non-Goals:**

- Achieve byte-for-byte or range-for-range parity with clangd in this change.
- Add fine-grained folding for every C++ syntax detail such as template parameter lists, requires-clauses, or attribute arguments before their value is proven.
- Introduce editor-specific behavior that only exists to satisfy one frontend.
- Add cross-file or index-backed folding behavior.

## Decisions

### 1. Split the folding-range pipeline into collection, normalization, and rendering

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

### 2. Keep rich internal categories, but only promise standard-compatible public kinds

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

### 3. Implement comment folding through lexical/source scanning, not AST

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

### 4. Rework preprocessor folding around complete branch blocks instead of the current half-open stack

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

### 5. Add dedicated directive-based collectors for macros and include/import groups

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

### 6. Folding-range output must be explicitly bound to client capabilities

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

### 7. Organize tests by source category and protocol behavior

Tests will be split into two dimensions:

- source-category unit tests: AST structure, comments, conditional compilation, multiline macros, `#pragma region`, and include/import groups
- protocol-behavior tests: `lineFoldingOnly`, `collapsedText` support, public kind mapping, and range limiting

In particular, the current [`tests/unit/feature/folding_range_tests.cpp`](/home/ykiko/C++/clice/tests/unit/feature/folding_range_tests.cpp) contains `Directive` and `PragmaRegion` cases that do not actually assert results. This change upgrades them into strong assertion-based tests.

Alternative considered:

- Rely mostly on manual editor validation. Rejected because folding details regress easily, especially for preprocessor handling and line-only rendering.

## Risks / Trade-offs

- [Client capabilities must flow from initialize state into request-time rendering] -> Mitigation: introduce a dedicated folding-options structure so session details do not leak broadly into the feature layer.
- [Inactive-branch and macro-definition ranges can be unstable around expansion locations] -> Mitigation: prefer spelling/main-file ranges and explicitly filter or special-case macro-expansion ranges when necessary.
- [Adding comments, macros, and include/import groups can increase the number of ranges quickly] -> Mitigation: implement stable sorting and `rangeLimit` trimming in the normalization layer.
- [Mapping public kinds back to standard values changes current metadata output] -> Mitigation: the folds themselves remain; the user-visible change is mostly in optional metadata, and tests plus change notes will make that explicit.
- [Multiple collectors may produce overlapping or duplicate ranges] -> Mitigation: normalize by source category and boundary rules so collectors do not amplify noise.

## Migration Plan

1. Refactor the internal folding-range data flow and add render options plus standard kind mapping.
2. Add the comment collector and assertion-backed tests for multiline comment folding.
3. Rewrite conditional-directive and `#pragma region` collection so `#if` branches close correctly through `#endif`.
4. Add multiline macro folding and grouped include/import collectors.
5. Wire folding client capabilities through initialize/request handling and add integration coverage.
6. Add `rangeLimit` trimming and regression cleanup after the new collectors are in place.

Rollback strategy:

- If protocol negotiation proves unstable, keep the new collectors but temporarily disable outward behavior changes tied to `collapsedText` or `rangeLimit`.
- If a particular new fold category proves noisy, roll it back collector-by-collector instead of reverting the entire folding-range refactor.

## Open Questions

- Should multiline macro folding cover only the macro body, or the full `#define NAME(...)` line plus body as one fold region?
- Should `rangeLimit` prioritize outer structure, top-of-file regions, or longer ranges when trimming results?
- For structural AST folds originating from macro expansion, should `clice` preserve current behavior or restrict itself to cases with stable spelling ranges only?
