#pragma once

#include "syntax/token.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A source file with inline `§` annotations stripped out. The stripped
/// text is the canonical coordinate space: every offset refers to the
/// content as the compiler sees it. The parser is mirrored byte-for-byte
/// by tools/snap/annotation.ts; grammar changes must land on both
/// sides.
struct AnnotatedSource {
    std::string content;
    llvm::StringMap<std::uint32_t> offsets;
    llvm::StringMap<LocalSourceRange> ranges;
    std::vector<std::uint32_t> nameless_offsets;

    static AnnotatedSource from(llvm::StringRef content);
};

struct AnnotatedSources {
    llvm::StringMap<AnnotatedSource> all_files;

    void add_source(llvm::StringRef file, llvm::StringRef content) {
        all_files.insert_or_assign(file, AnnotatedSource::from(content));
    }

    void add_sources(llvm::StringRef content);
};

}  // namespace clice
