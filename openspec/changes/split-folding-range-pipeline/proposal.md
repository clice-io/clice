## Why

`explore-improve-folding-range-support` combines several different concerns: upstream comparison work, baseline folding fixes, preprocessor extensions, and folding renderer behavior. The second design point in that change, splitting the folding-range pipeline into collection, normalization, and rendering, is the architectural slice that other work depends on and should be referenceable as its own proposal.

Follow-up discussion clarified that the existing internal `RawFoldingRange` shape is finished for this slice. The missing architectural part is not another raw-range redesign; it is an explicit folding options path so callers can request client-specific rendering behavior, starting with `line_folding_only`.

## What Changes

- Extract the pipeline-splitting work from `explore-improve-folding-range-support` into a standalone change focused on folding-range architecture.
- Treat the existing `RawFoldingRange` model as the settled internal collection contract for this change.
- Define a normalization phase that performs deterministic sorting, duplicate removal, and boundary validation before response generation.
- Define a folding options object, passed as `Opts`/`FoldingRangeOptions`, that configures rendering without changing collectors.
- Define a rendering phase that owns line/column shaping, including `line_folding_only`, and optional metadata emission instead of mixing those concerns into collectors.
- Preserve the current AST structural folding coverage while establishing extension points for future comment, directive, and capability-aware rendering work.

## Capabilities

### New Capabilities
- `folding-range-pipeline`: Provide a deterministic folding-range pipeline that separates collection, normalization, and rendering while preserving existing structural folds.

### Modified Capabilities
- None.

## Impact

- `src/feature/folding_ranges.cpp` will keep raw-range collection but gain explicit normalization/rendering boundaries and options-driven rendering.
- `src/feature/feature.h` will need a folding options type or equivalent public API extension so `line_folding_only` can be configured without changing collection.
- `tests/unit/feature/folding_range_tests.cpp` will need regression coverage for structural folds, deterministic ordering, and `line_folding_only` boundary shaping.
- `openspec/changes/explore-improve-folding-range-support/design.md` remains the source change from which this standalone proposal was extracted.
