#pragma once

#include <clang/Basic/SourceLocation.h>
#include <clang/AST/Decl.h>

namespace clice {

/// Checks whether the location is inside the main file.
bool is_inside_main_file(clang::SourceLocation loc, const clang::SourceManager& sm);
/// Checks whether the decl is a top-level decl in the main file.
bool is_clangd_top_level_decl(const clang::Decl* decl);

}  // namespace clice
