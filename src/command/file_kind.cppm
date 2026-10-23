module;

#include "support/prelude.h"

module clice:command.file_kind;

namespace clice {

/// The type clang assigns to a file by its extension (Types.def);
/// TY_INVALID when it has none — `.cuh`, which clang does not list, and
/// unknown suffixes alike.
clang::driver::types::ID suffix_type(llvm::StringRef path);

/// The clang language name of a file by its extension ("c++", "cuda",
/// ...), a C source or header taken as C++ under a C++ driver (`cxx`);
/// `.cuh`, missing from clang's table, is CUDA; the raw extension itself
/// when nothing maps it.
llvm::StringRef extension_language(llvm::StringRef path, bool cxx);

/// Whether the extension names a header — or nothing clang knows, which a
/// file only ever included usually is: a `.inc` or `.cuh`, the standard
/// library's extensionless headers.
bool is_header_path(llvm::StringRef path);

/// Whether the extension names a translation unit of a C-family language:
/// a source, preprocessed or a module interface, never a header.
bool is_source_path(llvm::StringRef path);

/// Whether the extension names a file clang knows that is no C-family
/// code: an assembler source, preprocessed or not, LLVM IR, a module file,
/// an object file.
bool is_other_language_path(llvm::StringRef path);

/// Whether the extension marks a fragment only ever included into a
/// translation unit — a `.def` list, a `.inc`, `.inl`, `.tpp`, `.ipp`,
/// `.tcc` or `.txx` body — which compiles only under its includer's
/// context.
bool is_context_header_path(llvm::StringRef path);

/// Whether clang parses a file of `type` as C-family code: what it accepts
/// but assembler-with-cpp, which it only preprocesses for the assembler,
/// and the IR, ASTs and module files it reads back.
bool is_c_family_type(clang::driver::types::ID type);

/// Check if a filename has a C/C++/ObjC/CUDA/etc. extension clang parses.
/// Returns false for .rc (Windows resource), .asm, .s and .S, .ll, .pcm, .def,
/// and other non-C-family files.
bool is_c_family_file(llvm::StringRef filename);

}  // namespace clice
