#pragma once

/// Content hashes of declaration units: the key of cross-TU lint
/// deduplication. Two units with equal content get the same unit-local
/// clang-tidy diagnostics in both translation units, so the checks need
/// to run on only one of them.

#include <compare>
#include <cstdint>
#include <format>
#include <string_view>
#include <vector>

#include "compile/compilation_unit.h"
#include "syntax/token.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/Basic/SourceLocation.h"

namespace clice {

/// A 128-bit xxh3 hash, ordered by value so every combination of hashes
/// sorts its inputs independently of node numbering and include order.
struct ContentHash {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    constexpr auto operator<=>(const ContentHash&) const = default;
};

/// A top-level declaration and everything written with it. Files are
/// partitioned into units: trivia between declarations belongs to the
/// following unit, a container's closing brace and the file's tail to the
/// last one.
struct ContentUnit {
    /// The unit's top-level nodes in the semantics table; several when
    /// declarators share tokens (`int a, b;`, `struct S {} s;`).
    llvm::SmallVector<std::uint32_t, 1> nodes;

    /// The declarations of the top-level nodes, in node order: what a
    /// traversal scope names to visit exactly this unit.
    llvm::SmallVector<const clang::Decl*, 1> decls;

    clang::FileID fid;

    LocalSourceRange range;

    /// Files a `#include` inside the unit's spelled span pulled in, whole.
    llvm::SmallVector<clang::FileID, 1> fragments;

    /// The units this one depends on: every declaration its nodes refer
    /// to, expanded to all redeclarations and, for templates, the whole
    /// family. Sorted, unique, never the unit itself.
    llvm::SmallVector<std::uint32_t, 4> deps;

    /// The entity of the first named declaration, 0 for a unit without
    /// one (`static_assert`, file-level `asm`, an empty namespace).
    std::uint64_t entity = 0;

    /// What the unit itself contributes. Template instantiations take no
    /// part: neither their references nor what the compiler materialized
    /// from them.
    ContentHash own;

    /// own combined with the content of every dependency, transitively.
    ContentHash content;

    /// One per instantiation the compiler materialized from a template of
    /// this unit: its entity, what of it was materialized, the content of
    /// what its body refers to and the entities it selected. Checks that
    /// look into instantiated bodies key on (content, element); nothing
    /// here propagates to the unit's dependents. Sorted, unique.
    llvm::SmallVector<ContentHash, 2> elements;
};

struct ContentTable {
    /// Sorted by (file, range.begin).
    std::vector<ContentUnit> units;

    /// One per file with units: the file's text and its units' content in
    /// source order.
    llvm::DenseMap<clang::FileID, ContentHash> digests;

    /// Compute the table of a completed compilation: a whole-TU semantics
    /// build with instantiations, transient like the index projection's.
    /// Elements are computed for the templates of the files `elements_in`
    /// accepts (every file when null): hashing what the compiler
    /// materialized is the bulk of the work, and a consumer that never
    /// checks a file has no use for its instantiations.
    static ContentTable compute(CompilationUnitRef unit,
                                llvm::function_ref<bool(clang::FileID)> elements_in = {});

    /// The unit owning `offset` of `fid`, null when the file has none.
    const ContentUnit* find(clang::FileID fid, std::uint32_t offset) const;
};

}  // namespace clice

/// 32 hex digits, high word first.
template <>
struct std::formatter<clice::ContentHash> : std::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(clice::ContentHash hash, FormatContext& context) const {
        return std::formatter<std::string_view>::format(
            std::format("{:016x}{:016x}", hash.high, hash.low),
            context);
    }
};
