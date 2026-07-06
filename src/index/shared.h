#pragma once

#include <cstdint>

#include "llvm/ADT/DenseMap.h"
#include "clang/Basic/SourceLocation.h"

namespace clice {

class CompilationUnitRef;

}

namespace clice::index {

template <typename T>
using Shared = llvm::DenseMap<clang::FileID, T>;

/// On-disk index blob schema version. The wire layout is derived from the
/// reflected in-memory types, so bump this whenever their serialized fields
/// change; loaders silently discard blobs carrying a different value (flatc
/// era blobs are already rejected by the kotatsu buffer identifier).
constexpr inline std::uint32_t index_format_version = 2;

}  // namespace clice::index
