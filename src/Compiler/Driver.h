#pragma once

#include "Compiler/Command.h"
#include "Support/Logging.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "clang/Driver/Driver.h"

namespace clice {

namespace {

namespace opt = llvm::opt;
namespace driver = clang::driver;

/// Checks if dash-dash (`--`) parsing is enabled. If enabled, all arguments
/// after a standalone `--` are treated as positional arguments (e.g., input files).
bool enable_dash_dash_parsing(const opt::OptTable& table);

/// Checks if grouped short options are enabled. If enabled, a short option group
/// like `-ab` is parsed as separate options `-a` and `-b`.
bool enable_grouped_short_options(const opt::OptTable& table);

/// Get the specific toolchain of given target, we mainly use it to get msvc toolchain.
const driver::ToolChain& get_toolchain(driver::Driver& driver,
                                       const opt::ArgList& Args,
                                       const llvm::Triple& Target);

template <auto MP1, auto MP2, auto MP3>
struct Thief {
    friend bool enable_dash_dash_parsing(const opt::OptTable& table) {
        return table.*MP1;
    }

    friend bool enable_grouped_short_options(const opt::OptTable& table) {
        return table.*MP2;
    }

    friend const driver::ToolChain& get_toolchain(driver::Driver& driver,
                                                  const opt::ArgList& args,
                                                  const llvm::Triple& target) {
        return (driver.*MP3)(args, target);
    }
};

template struct Thief<&opt::OptTable::DashDashParsing,
                      &opt::OptTable::GroupedShortOptions,
                      &driver::Driver::getToolChain>;

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

    inline static auto& option_table = clang::driver::getDriverOptTable();

    void set_arguments(llvm::ArrayRef<const char*> arguments) {
        if(getArgs().size() != 0) {
            std::abort();
        }

        this->arguments = arguments;
    }

    std::unique_ptr<llvm::opt::Arg> parse_one(unsigned& index) {
        /// Make sure we are not using
        assert(!enable_dash_dash_parsing(option_table));
        assert(!enable_grouped_short_options(option_table));
        return option_table.ParseOneArg(*this, index);
    }

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

                /// FIXME: When parsing fails, the parser may have encountered unknown
                /// arguments (e.g., options for a different compiler like nvcc).
                /// We should allow the user to provide a custom option registry
                /// (mainly for these pass-through arguments).
                ///
                /// This would let us ignore them correctly. For example, when
                /// parsing `nvcc --option-dir x.txt main.cpp`, our parser fails
                /// because it discards `--option-dir` but doesn't know it also
                /// consumes the next argument (`x.txt`).
                ///
                /// With a custom registry, we could register that `--option-dir`
                /// takes one argument, allowing us to skip both and continue
                /// parsing from `main.cpp`.
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

}  // namespace

}  // namespace clice
