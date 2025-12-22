#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice::toolchain {

enum class CompilerFamily {
    Unknown,
    GCC,      // Covers gcc, g++, cc, c++, and versioned/arch variants
    Clang,    // Covers clang, clang++, and versioned variants (excluding clang-cl)
    MSVC,     // Covers cl
    ClangCL,  // Covers clang-cl explicitly
    NVCC,     // Covers nvcc
    Intel,    // Covers icc, icpc, icx, dpcpp
    Zig,      // Covers zig cc / zig c++ (assumed GCC/Clang compatible for query)
};

struct QueryParams {
    llvm::StringRef file;
    llvm::StringRef directory;
    llvm::ArrayRef<const char*> arguments;
    llvm::function_ref<const char*(const char*)> callback;
};

/// Query the toolchain info and return the full arguments, the returned arguments should be
/// converted to `clang::CompilerInvocation::CreateFromArgs` directly.
std::vector<const char*> query_toolchain(const QueryParams& params);

CompilerFamily driver_family(llvm::StringRef driver);

/// Query g++ or mingw toolchain info. We detect the target and corresponding
/// gcc toolchain install path as default behavior.
std::vector<const char*> query_gcc_toolchain(const QueryParams& params);

/// Query clang++ or any clang based toolchain, e.g. zig cc/c++. We query
/// the full cc1 command of clang toolchain as default.
/// TODO: Is armclang also compatible?
std::vector<const char*> query_clang_toolchain(const QueryParams& params);

/// Query the msvc or clang-cl toolchain, default behavior only adds the
/// target and includes info.
std::vector<const char*> query_msvc_toolchain(const QueryParams& params);

/// FIXME: To be implemented
std::vector<const char*> query_nvcc_toolchain(const QueryParams& params);

}  // namespace clice::toolchain
