module clice:command.toolchains.own_options;

import :command.argument;
import :support.prelude;

namespace clice {

/// A kotatsu option table built at run time from spellings: the options
/// of a compiler clang's table does not read as the compiler does. Every
/// spelling, written with its prefix (`--gpu-architecture=`, `-I`,
/// `/fsycl`), is an option of the table, and one naming an option, whose
/// ids run from `first_option` to the table's end, is its alias: a parse
/// yields that option whichever spelling a command wrote. A `/` spelling
/// takes cl's `-` too.
class OptionSpellings {
public:
    struct Entry {
        std::string name;

        cli::OptionKind kind;

        /// The option the spelling names, none for an option of its own,
        /// whose id is the spelling's place after the named options.
        std::uint32_t option;

        /// The families that read the spelling (family_bit), all by
        /// default.
        std::uint32_t visibility = ~0u;
    };

    /// The ids of the input and of an unknown option.
    constexpr static std::uint32_t input = 1;
    constexpr static std::uint32_t unknown = 2;

    constexpr static std::uint32_t first_option = 3;

    /// The options `first_option` to `end` - 1, named by `spellings`.
    OptionSpellings(std::uint32_t end, llvm::ArrayRef<Entry> spellings);

    OptionSpellings(const OptionSpellings&) = delete;

    const cli::OptTable& table() const {
        return opt_table;
    }

private:
    static std::vector<cli::Option> build(std::uint32_t end,
                                          llvm::ArrayRef<Entry> spellings,
                                          llvm::StringSaver& names);

private:
    llvm::BumpPtrAllocator allocator;

    llvm::StringSaver names{allocator};

    std::vector<cli::Option> options;

    cli::OptTable opt_table;
};

/// The visibility bit of `family` among an OptionSpellings' spellings.
constexpr std::uint32_t family_bit(CompilerFamily family) {
    return 1u << static_cast<unsigned>(family);
}

/// The arguments of a command of `family` as clice reads them: as clang's
/// table parses them under `options`, but an argument clang's table does
/// not know (an unknown option, a `/` spelling it takes for an input) as an
/// option of the compiler's own that clang names otherwise (Intel's
/// `/fsycl`, `-qopenmp`), with clang's id, or not at all when the option
/// only concerns what the build writes (GCC's `-fmodule-mapper=`).
std::vector<std::expected<cli::ParsedArg, cli::Error>>
    parse_arguments(llvm::ArrayRef<std::string> args,
                    CompilerFamily family,
                    const cli::ParseOptions& options);

}  // namespace clice
