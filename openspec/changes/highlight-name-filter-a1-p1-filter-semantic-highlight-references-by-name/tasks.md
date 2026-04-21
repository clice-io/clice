## Request Metadata

- Proposal Title: Filter semantic highlight references by name eligibility
- Canonical Request Title: `fix(semantic-tokens): filter ineligible highlight references`
- Conventional Title: `fix(semantic-tokens)`

## 1. Declaration-Name Eligibility

- [ ] 1.1 Add a reusable declaration-name eligibility helper in `src/semantic/ast_utility.h` and `src/semantic/ast_utility.cpp` that mirrors clangd's `canHighlightName` behavior.
- [ ] 1.2 Update `SemanticTokensCollector::handleDeclOccurrence()` in `src/feature/semantic_tokens.cpp` to skip reference-token emission when the target declaration name is not eligible for source highlighting.

## 2. Regression Coverage

- [ ] 2.1 Add a semantic-token unit test for one unsupported declaration-name reference repro that currently produces an ineligible highlight.
- [ ] 2.2 Add constructor and destructor semantic-token regression coverage that confirms eligible special-member references still emit tokens with the existing classification and modifiers.

## 3. Verification

- [ ] 3.1 Run the targeted semantic-token test suite and confirm the new filter changes only reference-token behavior for the covered repros.
