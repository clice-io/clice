#pragma once

#include <cassert>
#include <memory>

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

}  // namespace clice
