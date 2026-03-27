#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Allocator.h"

namespace clice {

class ArgumentParser final : public llvm::opt::ArgList {
public:
    ArgumentParser(llvm::BumpPtrAllocator* allocator) : allocator(allocator) {}

    ~ArgumentParser() {
        /// We never use the private `Args` field, so make sure it's empty.
        if(getArgs().size() != 0) {
            std::abort();
        }
    }

    const char* getArgString(unsigned index) const override {
        return arguments[index];
    }

    unsigned getNumInputArgStrings() const override {
        return arguments.size();
    }

    const char* MakeArgStringRef(llvm::StringRef s) const override {
        auto p = allocator->Allocate<char>(s.size() + 1);
        std::ranges::copy(s, p);
        p[s.size()] = '\0';
        return p;
    }

    /// Set visibility mask for option parsing. The default (~0u) accepts all
    /// options. Pass a narrower mask to exclude option groups — e.g. exclude
    /// MSVC cl.exe-style /U, /D, /I options that would otherwise misparse
    /// Unix absolute paths like /Users/... on macOS.
    void set_visibility(unsigned mask) { visibility_mask = mask; }

    /// Parse a single argument at the given index. Defined out-of-line in
    /// argument_parser.cpp to isolate the heavy clang driver option table include.
    std::unique_ptr<llvm::opt::Arg> parse_one(unsigned& index);

    void parse(llvm::ArrayRef<const char*> arguments, const auto& on_parse, const auto& on_error) {
        this->arguments = arguments;

        unsigned it = 0;
        while(it != arguments.size()) {
            llvm::StringRef s = arguments[it];

            if(s.empty()) [[unlikely]] {
                it += 1;
                continue;
            }

            auto prev = it;
            auto arg = parse_one(it);
            assert(it > prev && "parser failed to consume argument");

            if(!arg) [[unlikely]] {
                assert(it >= arguments.size() && "unexpected parser error!");
                assert(it - prev - 1 && "no missing arguments!");

                on_error(prev, it - prev - 1);
                break;
            }

            on_parse(std::move(arg));
        }
    }

private:
    llvm::BumpPtrAllocator* allocator;
    unsigned visibility_mask = ~0u;

    llvm::ArrayRef<const char*> arguments;
};

/// Check if an option is a codegen-only flag that doesn't affect frontend
/// semantics (parsing, diagnostics, code completion). These are pure
/// backend/linker concerns irrelevant to an LSP server.
///
/// Note: options that DO affect semantics are intentionally kept:
///   -fno-exceptions, -fno-rtti, -std=*, -march=*, -fsanitize=*, -O*, -W*
///
/// Defined out-of-line in argument_parser.cpp (needs clang driver option IDs).
bool is_codegen_option(unsigned id, const llvm::opt::Option& opt);

/// Options that are completely irrelevant to an LSP and should be discarded
/// (input/output, PCH building, dependency scan, C++ modules).
bool is_discarded_option(unsigned id);

/// User-content options that go into the per-file patch rather than the
/// shared canonical command: -I, -D, -U, -include, -isystem, -iquote, -idirafter.
bool is_user_content_option(unsigned id);

/// Subset of user-content options that are include-path flags
/// (-I, -isystem, -iquote, -idirafter) — used for path absolutization.
bool is_include_path_option(unsigned id);

/// Check if this is the -Xclang pass-through option.
bool is_xclang_option(unsigned id);

/// Options that affect system path discovery and should be included in the
/// toolchain cache key. Only these flags are passed to the toolchain query.
bool is_toolchain_option(unsigned id);

/// Get the option ID for a specific argument string.
std::optional<std::uint32_t> get_option_id(llvm::StringRef argument);

/// Get the resource directory for clang builtin headers. Computed once
/// from the current executable path using Driver::GetResourcesPath.
llvm::StringRef resource_dir();

/// Format an argument list as a human-readable string: "[arg1 arg2 ...]".
std::string print_argv(llvm::ArrayRef<const char*> args);

/// Return the visibility mask to exclude MSVC cl.exe-style options (/U, /D,
/// /I, etc.) unless the driver is cl.exe.  This prevents Unix absolute paths
/// like /Users/... from being misparsed as /U sers/... on macOS/Linux.
/// Defined out-of-line in argument_parser.cpp (needs ClangVisibility enum).
unsigned default_visibility(llvm::StringRef driver);

}  // namespace clice
