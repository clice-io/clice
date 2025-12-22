#pragma once

#include "AST/SourceCode.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice::testing {

struct AnnotatedSource {
    std::string content;
    /// All named offsets
    llvm::StringMap<std::uint32_t> offsets;

    llvm::StringMap<LocalSourceRange> ranges;

    std::vector<std::uint32_t> nameless_offsets;

    /// Point Annotation:
    /// - $(key): Marks a single point.
    ///
    /// Range Annotation:
    /// - @key[...content...]: Marks a range.
    ///
    /// A range annotation for 'key' creates both a `ranges["key"]` and an `offsets["key"]`
    /// (pointing to the start).
    static AnnotatedSource from(llvm::StringRef content);
};

struct AnnotatedSources {
    /// All sources file in the compilation.
    llvm::StringMap<AnnotatedSource> all_files;

    void add_source(llvm::StringRef file, llvm::StringRef content) {
        all_files.try_emplace(file, AnnotatedSource::from(content));
    }

    /// Add sources to the params, use `#[filename]` to mark
    /// a new file start. For example
    ///
    /// ```cpp
    /// #[test.h]
    /// int foo();
    ///
    /// #[main.cpp]
    /// #include "test.h"
    /// int x = foo();
    /// ```
    void add_sources(llvm::StringRef content);
};

}  // namespace clice::testing
