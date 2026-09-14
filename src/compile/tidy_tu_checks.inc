// Checks whose findings a single declaration cannot vouch for: they keep
// state across the translation unit, match the TU itself, follow calls into
// other declarations, or read TU-wide facts (`isUsed`, the macro table). A
// unit-local content hash cannot key their results, so cross-TU
// deduplication never claims them — they run in every TU and the master
// merges identical findings. Audited against clang-tools-extra/clang-tidy
// with .claude/skills/upgrade-llvm/tidy_audit.py; re-audit on an LLVM
// upgrade.
//
// TU_LEVEL_CHECK(name, why)
TU_LEVEL_CHECK("misc-unused-using-decls", "uses are counted over the whole TU")
TU_LEVEL_CHECK("misc-unused-alias-decls", "uses are counted over the whole TU")
TU_LEVEL_CHECK("bugprone-forward-declaration-namespace", "pairs declarations across the TU")
TU_LEVEL_CHECK("misc-confusable-identifiers", "compares identifiers across the TU")
TU_LEVEL_CHECK("misc-no-recursion", "matches the translation unit, builds a call graph")
TU_LEVEL_CHECK("misc-new-delete-overloads", "pairs new/delete declared in different declarations")
TU_LEVEL_CHECK("misc-include-cleaner", "uses are counted over the whole TU")
TU_LEVEL_CHECK("misc-header-include-cycle", "looks at the TU's whole include graph")
TU_LEVEL_CHECK("readability-inconsistent-declaration-parameter-name", "picks the primary location by redeclaration order in the TU")
TU_LEVEL_CHECK("bugprone-exception-escape", "follows calls, including instantiated callees")
TU_LEVEL_CHECK("openmp-exception-escape", "follows calls, including instantiated callees")
TU_LEVEL_CHECK("modernize-use-equals-delete", "reads TU-wide isUsed()")
TU_LEVEL_CHECK("portability-template-virtual-member-function", "reports members no TU instantiated")
TU_LEVEL_CHECK("misc-const-correctness", "ExprMutationAnalyzer follows instantiated callees")
TU_LEVEL_CHECK("performance-unnecessary-value-param", "ExprMutationAnalyzer follows instantiated callees")
TU_LEVEL_CHECK("performance-for-range-copy", "ExprMutationAnalyzer follows instantiated callees")
TU_LEVEL_CHECK("bugprone-infinite-loop", "ExprMutationAnalyzer follows instantiated callees")
TU_LEVEL_CHECK("bugprone-redundant-branch-condition", "ExprMutationAnalyzer follows instantiated callees")
TU_LEVEL_CHECK("readability-use-anyof-allof", "ExprMutationAnalyzer follows instantiated callees")
TU_LEVEL_CHECK("cppcoreguidelines-special-member-functions", "merges a template with its specializations by location")
TU_LEVEL_CHECK("bugprone-unsafe-functions", "reads macros defined anywhere in the TU")
TU_LEVEL_CHECK("readability-identifier-naming", "reads the TU's macro table for renaming")
TU_LEVEL_CHECK("bugprone-reserved-identifier", "reads the TU's macro table for renaming")
// The pruned groups traverse claimed declarations as the translation
// unit's direct children, so a namespace or linkage specification is never
// an ancestor there: checks that match those containers or ask for them
// as ancestors run whole.
TU_LEVEL_CHECK("llvm-redundant-casting", "asks for namespace and TU ancestors")
TU_LEVEL_CHECK("readability-redundant-casting", "asks for namespace and TU ancestors")
TU_LEVEL_CHECK("abseil-no-namespace", "matches namespaces")
TU_LEVEL_CHECK("abseil-no-internal-dependencies", "asks for a namespace parent")
TU_LEVEL_CHECK("llvmlibc-implementation-in-namespace", "matches the translation unit's children")
TU_LEVEL_CHECK("google-objc-function-naming", "asks for a namespace ancestor")
TU_LEVEL_CHECK("modernize-avoid-c-style-cast", "asks for a linkage specification ancestor")
TU_LEVEL_CHECK("google-readability-casting", "asks for a linkage specification ancestor")
TU_LEVEL_CHECK("modernize-macro-to-enum", "asks for the TU as parent")
TU_LEVEL_CHECK("cppcoreguidelines-macro-to-enum", "asks for the TU as parent")
TU_LEVEL_CHECK("modernize-use-using", "asks for a linkage specification ancestor")
TU_LEVEL_CHECK("readability-redundant-declaration", "asks for a linkage specification parent")
TU_LEVEL_CHECK("llvm-namespace-comment", "matches namespaces")
TU_LEVEL_CHECK("google-readability-namespace-comments", "matches namespaces")
TU_LEVEL_CHECK("misc-static-initialization-cycle", "matches the translation unit")
TU_LEVEL_CHECK("misc-anonymous-namespace-in-header", "matches namespaces")
TU_LEVEL_CHECK("fuchsia-header-anon-namespaces", "matches namespaces")
TU_LEVEL_CHECK("google-build-namespaces", "matches namespaces")
TU_LEVEL_CHECK("cert-dcl59-cpp", "matches namespaces")
TU_LEVEL_CHECK("bugprone-std-namespace-modification", "asks for namespace ancestors")
TU_LEVEL_CHECK("cert-dcl58-cpp", "asks for namespace ancestors")
