## Why

`explore-improve-folding-range-support` combines several different concerns: upstream comparison work, baseline folding fixes, preprocessor extensions, and an internal refactor. The second design point in that change, splitting the folding-range pipeline into collection, normalization, and rendering, is the architectural slice that other work depends on and should be referenceable as its own proposal.

## What Changes

- Extract the pipeline-splitting work from `explore-improve-folding-range-support` into a standalone change focused on folding-range architecture.
- Introduce an internal raw folding-range model so collectors no longer emit final LSP objects directly.
- Define a normalization phase that performs deterministic sorting, duplicate removal, and boundary validation before response generation.
- Define a rendering phase that owns line/column shaping and optional metadata emission instead of mixing those concerns into collectors.
- Preserve the current AST structural folding coverage while establishing extension points for future comment, directive, and capability-aware rendering work.

## Capabilities

### New Capabilities
- `folding-range-pipeline`: Provide a deterministic folding-range pipeline that separates collection, normalization, and rendering while preserving existing structural folds.

### Modified Capabilities
- None.

## Impact

- `src/feature/folding_ranges.cpp` will be refactored around raw-range collection, normalization, and rendering boundaries.
- Folding-related helper types may be introduced near the folding feature implementation.
- `tests/unit/feature/folding_range_tests.cpp` will need regression coverage for structural folds and deterministic ordering.
- `openspec/changes/explore-improve-folding-range-support/design.md` remains the source change from which this standalone proposal was extracted.
