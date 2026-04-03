## 1. Outline Traversal

- [ ] 1.1 Replace the generic document symbol collector with a dedicated outline traversal that can distinguish skip / only-decl / only-children / decl-and-children behavior.
- [ ] 1.2 Filter out function-local declarations, implicit entities, and implicit template instantiations while preserving namespace, type, enum, field, and member hierarchy.
- [ ] 1.3 Implement explicit specialization and explicit instantiation behavior so written specializations can expose children and explicit instantiations remain leaf symbols.

## 2. Macro And Range Handling

- [ ] 2.1 Build macro invocation container nodes for declarations expanded from main-file macro calls.
- [ ] 2.2 Add post-processing to stabilize sibling ordering and enforce `selectionRange`-within-`range` plus parent-range-contains-child-range invariants.
- [ ] 2.3 Confirm whether the current protocol layer can carry deprecated document symbol metadata and either implement it here or record a follow-up.

## 3. Validation

- [ ] 3.1 Rewrite unit tests to assert symbol names, kinds, hierarchy, and range invariants instead of only total node counts.
- [ ] 3.2 Add regression cases covering local declarations, explicit specialization/instantiation, and macro-expanded declarations.
- [ ] 3.3 Extend integration coverage so the server request path exercises representative document symbol responses.
