module clice;

import :command.argument;
import :command.toolchains.own_options;

namespace clice {

namespace {

constexpr std::string_view slash_prefixes[] = {"/", "-"};

/// The families an own option is written by: every one, Intel's icpx and
/// icx elsewhere, Intel's drivers that read cl's options (icx-cl, dpcpp-cl
/// and icx on Windows, read as clang-cl: driver_family), and cl.
namespace families {

constexpr std::uint32_t all = ~0u;
constexpr std::uint32_t intel = family_bit(CompilerFamily::Intel);
constexpr std::uint32_t intel_cl = family_bit(CompilerFamily::ClangCL);
constexpr std::uint32_t msvc = family_bit(CompilerFamily::MSVC);

}  // namespace families

/// The clang option each own option reads as, by its position.
constexpr unsigned clang_ids[] = {
#define OWN_OPTION(SPELLING, KIND, CLANG, FAMILIES) clang_options::OPT_##CLANG,
#include "command/toolchains/own_options.inc"
#undef OWN_OPTION
};

const cli::OptTable& own_options() {
    using enum cli::OptionKind;
    const static OptionSpellings table(OptionSpellings::first_option,
                                       {
#define OWN_OPTION(SPELLING, KIND, CLANG, FAMILIES) {SPELLING, KIND, 0, families::FAMILIES},
#include "command/toolchains/own_options.inc"
#undef OWN_OPTION
                                       });
    return table.table();
}

}  // namespace

OptionSpellings::OptionSpellings(std::uint32_t end, llvm::ArrayRef<Entry> spellings) :
    options(build(end, spellings, names)), opt_table(options) {}

std::vector<cli::Option> OptionSpellings::build(std::uint32_t end,
                                                llvm::ArrayRef<Entry> spellings,
                                                llvm::StringSaver& names) {
    std::vector<cli::Option> options = {cli::Option::input(input), cli::Option::unknown(unknown)};
    // An option of no spelling of its own, a group no argument matches.
    for(std::uint32_t id = first_option; id < end; id += 1) {
        options.push_back(
            cli::Option::unaliased_one(cli::pfx_none, "<option>", id, cli::OptionKind::Group, 0));
    }
    for(auto& spelling: spellings) {
        llvm::StringRef name = spelling.name;
        auto prefixes = name.starts_with("--")  ? cli::pfx_double
                        : name.starts_with("/") ? std::span<const std::string_view>(slash_prefixes)
                                                : cli::pfx_dash;
        auto option = cli::Option::unaliased_one(prefixes,
                                                 names.save(name),
                                                 static_cast<std::uint32_t>(options.size() + 1),
                                                 spelling.kind,
                                                 0,
                                                 "",
                                                 "",
                                                 0,
                                                 0,
                                                 spelling.visibility);
        options.push_back(spelling.option ? option.alias_of(spelling.option) : option);
    }
    return options;
}

std::vector<std::expected<cli::ParsedArg, cli::Error>>
    parse_arguments(llvm::ArrayRef<std::string> args,
                    CompilerFamily family,
                    const cli::ParseOptions& options) {
    auto own_parse = cli::ParseOptions{.visibility = family_bit(family)};
    std::vector<std::expected<cli::ParsedArg, cli::Error>> parsed;
    // Each argument parses where the one before it ends, by clang's table
    // or by the compiler's own, which may take a different number of
    // tokens.
    auto place = [](auto& result, std::uint32_t from) {
        if(result) {
            result->index += from;
            result->next_index += from;
        } else {
            result.error().begin += from;
        }
    };
    std::uint32_t i = 0;
    while(i < args.size()) {
        auto rest = args.drop_front(i);
        if(options.dash_dash_parsing && rest.front() == "--") {
            for(auto result: clang_options::table().parse(rest, options)) {
                place(result, i);
                parsed.push_back(std::move(result));
            }
            break;
        }
        auto result = *clang_options::table().parse(rest, options).begin();
        if(result &&
           (result->id == clang_options::OPT_UNKNOWN || result->id == clang_options::OPT_INPUT)) {
            auto own = *own_options().parse(rest, own_parse).begin();
            if(own && own->id >= OptionSpellings::first_option) {
                auto from = i;
                i += own->next_index;
                if(auto id = clang_ids[own->id - OptionSpellings::first_option]) {
                    // Spelled as clang spells it: none of clang's aliases
                    // is what the command wrote (written_alias).
                    own->id = id;
                    own->spelling = clang_options::table().option(id)->prefixed_name();
                    place(own, from);
                    parsed.push_back(std::move(own));
                }
                continue;
            }
        }
        if(!result) {
            place(result, i);
            parsed.push_back(std::move(result));
            break;
        }
        place(result, i);
        i = result->next_index;
        parsed.push_back(std::move(result));
    }
    return parsed;
}

}  // namespace clice
