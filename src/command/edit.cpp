module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument;
import :command.edit;
import :command.normalize;
import :command.nvcc;
import :command.table;
import :support.logging;

namespace clice {

namespace ranges = std::ranges;

/// An appended -gencode adds its architecture next to the base's, the way
/// nvcc itself accumulates them — resolve the arch flags to the newest.
static void collapse_gpu_arch_args(std::vector<LocalArg>& args) {
    llvm::SmallVector<std::pair<ArchFlagKind, llvm::StringRef>> sequence;
    llvm::SmallVector<std::size_t> positions;
    for(std::size_t i = 0; i < args.size(); i += 1) {
        auto& arg = args[i];
        std::optional<ArchFlagKind> kind;
        switch(arg.opt_id) {
            case option::OPT_offload_arch_EQ: kind = ArchFlagKind::OffloadArch; break;
            case option::OPT_no_offload_arch_EQ: kind = ArchFlagKind::NoOffloadArch; break;
        }
        if(kind && arg.values.size() == 1) {
            sequence.push_back({*kind, arg.values[0]});
            positions.push_back(i);
        }
    }

    auto dropped = collapse_gpu_archs(sequence);
    if(!dropped) {
        return;
    }
    for(auto index: *dropped | std::views::reverse) {
        args.erase(args.begin() + positions[index]);
    }
}

std::string CommandEdits::key() const {
    std::string key;
    auto append_section = [&](llvm::ArrayRef<std::string> section) {
        for(auto& item: section) {
            key += item;
            key += '\0';
        }
        key += '\1';
    };
    for(auto& edit: edits) {
        key += edit.kind == CommandEdit::Kind::Remove ? 'r' : 'a';
        key += edit.directory.str();
        key += '\0';
        append_section(edit.flags);
    }
    append_section(extra_prepend);
    append_section(extra_append);
    key += query_toolchain ? 'q' : 'u';
    return key;
}

CommandID
    apply_edits(CommandTable& commands, FileTable& files, CommandID id, const CommandEdits& edits) {
    if(edits.empty()) {
        return id;
    }

    const auto& base = commands.get(id);
    bool is_nvcc = base.family == CompilerFamily::NVCC;

    /// Rule flags for an NVCC entry arrive in the same nvcc spellings as
    /// the command they edit — rewrite them like the command itself.
    /// Appends translate as one command edit, so an appended default state
    /// (`-rdc=false` over an rdc base) cancels the base's translated state
    /// instead of vanishing. Relative paths in the flags, and in the files
    /// they name, are relative to the rule's `anchor`.
    auto translate_rule_flags =
        [&](llvm::ArrayRef<std::string> rule_flags, const Spelling& anchor, bool edit) {
            std::vector<std::string> flags(rule_flags.begin(), rule_flags.end());
            if(flags.empty() || !is_nvcc) {
                return flags;
            }
            llvm::BumpPtrAllocator allocator;
            llvm::StringSaver saver(allocator);
            llvm::SmallVector<const char*, 32> argv = {base.driver};
            for(auto& flag: flags) {
                argv.push_back(flag.c_str());
            }
            Normalizer{.commands = commands, .files = files}.expand_response_files(argv,
                                                                                   anchor,
                                                                                   base.family,
                                                                                   saver);
            auto translated = translate_nvcc_command(argv, anchor.str(), edit);
            flags.assign(std::make_move_iterator(translated.begin() + 1),
                         std::make_move_iterator(translated.end()));
            return flags;
        };

    auto remove_parse_options =
        kota::option::ParseOptions{.visibility = family_visibility(base.family)};

    /// One remove pattern: the option it names, and whether a `*` joined
    /// to the option's name takes every option spelled with that prefix
    /// (`-W*` takes -Wall and -Wextra, options of their own).
    struct RemovePattern {
        kota::option::ParsedArg option;
        bool prefix = false;
    };

    /// Parse one rule's remove list into option patterns. The parsed
    /// patterns view the translated spellings, which therefore outlive
    /// every match below.
    std::deque<std::vector<std::string>> remove_storage;
    auto parse_removes = [&](llvm::ArrayRef<std::string> flags, const Spelling& anchor) {
        std::vector<std::string> remove_source(flags.begin(), flags.end());
        if(is_nvcc) {
            /// A wildcard arch removal (`-arch=*`, `--generate-code=*`) must
            /// clear whichever form the translated base carries: numeric archs
            /// become `--cuda-gpu-arch=`, non-numeric selections persist as
            /// `-arch=` probe tokens — rewrite to both wildcards.
            for(std::size_t i = 0; i < remove_source.size(); i += 1) {
                llvm::StringRef flag = remove_source[i];
                for(llvm::StringRef spelling:
                    {"-arch", "--gpu-architecture", "-gencode", "--generate-code"}) {
                    bool joined =
                        flag.starts_with(spelling) && flag.substr(spelling.size()) == "=*";
                    bool separate = flag == spelling && i + 1 < remove_source.size() &&
                                    remove_source[i + 1] == "*";
                    if(!joined && !separate) {
                        continue;
                    }
                    if(separate) {
                        remove_source.erase(remove_source.begin() + i + 1);
                    }
                    remove_source[i] = "--cuda-gpu-arch=*";
                    remove_source.insert(remove_source.begin() + i + 1, "-arch=*");
                    i += 1;
                    break;
                }
            }
        }
        /// Remove patterns are an independent list, not one command: translated
        /// whole, nvcc's last-wins would swallow every alternative value of a
        /// stateful option but the last. Each pattern translates alone —
        /// standalone, so it reproduces exactly the flags the base translation
        /// emitted — pairing a separate value token (never dash-led) with its
        /// spelling.
        auto& remove_flags = remove_storage.emplace_back();
        if(is_nvcc) {
            for(std::size_t i = 0; i < remove_source.size(); i += 1) {
                std::size_t count = 1;
                if(llvm::StringRef(remove_source[i]).starts_with("-") &&
                   i + 1 < remove_source.size() &&
                   !llvm::StringRef(remove_source[i + 1]).starts_with("-")) {
                    count = 2;
                }
                auto pattern = translate_rule_flags(llvm::ArrayRef(remove_source).slice(i, count),
                                                    anchor,
                                                    /*edit=*/false);
                remove_flags.insert(remove_flags.end(),
                                    std::make_move_iterator(pattern.begin()),
                                    std::make_move_iterator(pattern.end()));
                i += count - 1;
            }
        } else {
            remove_flags = std::move(remove_source);
        }
        std::vector<RemovePattern> removes;
        auto add = [&](const kota::option::ParsedArg& parsed, llvm::StringRef written) {
            auto& remove = removes.emplace_back(RemovePattern{
                .option = parsed,
                .prefix = written.consume_back("*") && written == parsed.spelling,
            });
            // Anchored like the base command's paths, so a relative value
            // names the file the rule's configuration means; the `*`
            // wildcard stays a wildcard.
            for(auto& value: remove.option.values) {
                if(value != "*") {
                    value = anchored(commands, remove.option.id, value, anchor);
                }
            }
        };
        for(auto parsed: option::table().parse(remove_flags, remove_parse_options)) {
            if(!parsed.has_value()) {
                // An option written without its value, last in the list
                // (`-include`), removes it with every value.
                auto& wildcard = remove_storage.emplace_back(
                    std::vector<std::string>{remove_flags[parsed.error().index], "*"});
                auto retried = *option::table().parse(wildcard, remove_parse_options).begin();
                if(!retried) {
                    LOG_WARN("A rule removes {}, which takes several values: write them",
                             wildcard[0]);
                    continue;
                }
                add(*retried, wildcard[0]);
            } else if(parsed->id == option::OPT_INPUT) {
                LOG_WARN("A rule removes {}, which names no option", remove_flags[parsed->index]);
            } else {
                add(*parsed, remove_flags[parsed->index]);
            }
        }
        return removes;
    };

    /// Whether a remove pattern names `arg` (a base Arg or an appended
    /// LocalArg).
    auto removes_arg = [](const RemovePattern& wanted, const auto& arg) {
        auto& remove = wanted.option;
        // Among options of the remove's kind: `-W*` takes every warning,
        // not the preprocessor's `-Wp,-D...`.
        if(wanted.prefix && arg.cls == classify_option(remove.id, {})) {
            // As the argument renders: its option's name, with the value
            // joined to it when the option joins one (`-Wno-unused`).
            Arg view;
            if constexpr(requires { arg.view(); }) {
                view = arg.view();
            } else {
                view = arg;
            }
            std::string start;
            render_arg(view, [&](std::string_view fragment) {
                if(start.empty()) {
                    start = fragment;
                }
            });
            if(llvm::StringRef(start).starts_with(remove.spelling)) {
                return true;
            }
        }
        if(remove.id != arg.opt_id) {
            return false;
        }
        /// All unknown options share one id; their identity is the
        /// spelling (NVCC probe flags persist as unknown tokens). A
        /// trailing `=*` wildcards the value part, mirroring the
        /// known-option value wildcard below.
        if(arg.opt_id == option::OPT_UNKNOWN) {
            llvm::StringRef pattern = remove.spelling;
            bool wildcard = pattern.consume_back("*") && pattern.ends_with("=");
            return wildcard ? llvm::StringRef(arg.spelling).starts_with(pattern)
                            : arg.spelling == llvm::StringRef(remove.spelling);
        }
        if(remove.values.size() == 1 && remove.values[0] == "*") {
            return true;
        }
        return ranges::equal(arg.values, remove.values, [](const char* a, std::string_view b) {
            return std::string_view(a) == b;
        });
    };

    /// Parse an edit list into structured args, anchoring path values at
    /// `anchor` like the load pipeline. Unknown tokens keep the user's
    /// spelling and stay renderable (the user asked for them explicitly) —
    /// including input-classified ones: an edit cannot name the entry's
    /// input, so such a token is really the separate value of an option the
    /// table does not know.
    auto parse_edit = [&](llvm::ArrayRef<std::string> edit_flags,
                          const Spelling& anchor,
                          std::vector<LocalArg>& out) {
        std::vector<std::string> flags(edit_flags.begin(), edit_flags.end());
        for(auto& parsed: option::table().parse(flags, remove_parse_options)) {
            auto& local = out.emplace_back(commands.local_arg(parsed, flags));
            if(local.opt_id == option::OPT_UNKNOWN) {
                local.cls = ArgClass::Semantic;
            }
            for(auto& value: local.values) {
                value = anchored(commands, local.opt_id, value, anchor).data();
            }
        }
    };

    std::vector<RemovePattern> remove_args;
    std::vector<LocalArg> append_args;
    for(auto& edit: edits.edits) {
        if(edit.kind == CommandEdit::Kind::Remove) {
            auto removes = parse_removes(edit.flags, edit.directory);
            // A remove reaches the appends before it, so a later rule can
            // take back what an earlier one added.
            llvm::erase_if(append_args, [&](const LocalArg& local) {
                return llvm::any_of(removes, [&](const RemovePattern& remove) {
                    return removes_arg(remove, local);
                });
            });
            remove_args.insert(remove_args.end(), removes.begin(), removes.end());
        } else {
            parse_edit(translate_rule_flags(edit.flags, edit.directory, /*edit=*/true),
                       edit.directory,
                       append_args);
        }
    }
    auto entry_directory = Spelling::absolute(base.directory);
    parse_edit(edits.extra_append, entry_directory, append_args);

    std::vector<LocalArg> prepend_args;
    parse_edit(edits.extra_prepend, entry_directory, prepend_args);

    auto matches_remove = [&](const Arg& arg) {
        return llvm::any_of(remove_args,
                            [&](const RemovePattern& remove) { return removes_arg(remove, arg); });
    };

    /// Rebuild the sequence: prepends first, base args with removes
    /// cancelled, appends, then the input slot — so an append always takes
    /// effect for the compile, even over a flag the CDB writes after its
    /// input. Only a -x the CDB writes after the input stays after the slot:
    /// it governs no input before it.
    std::vector<LocalArg> edited;
    std::vector<LocalArg> trailing_selectors;
    edited.reserve(prepend_args.size() + base.args.size() + append_args.size());
    for(auto& local: prepend_args) {
        edited.push_back(local);
    }
    bool after_slot = false;
    for(auto& arg: base.args) {
        if(arg.cls == ArgClass::Input) {
            after_slot = true;
            continue;
        }
        if(matches_remove(arg)) {
            continue;
        }
        auto& into = after_slot && arg.opt_id == option::OPT_x ? trailing_selectors : edited;
        into.push_back({
            .opt_id = arg.opt_id,
            .cls = arg.cls,
            .spelling = arg.spelling,
            .values = {arg.values.begin(), arg.values.end()}
        });
    }
    llvm::append_range(edited, append_args);
    edited.push_back({.opt_id = option::OPT_INPUT, .cls = ArgClass::Input});
    llvm::append_range(edited, trailing_selectors);

    /// An appended -gencode adds its architecture next to the base's, the
    /// way nvcc itself accumulates them — resolve them to the newest.
    if(is_nvcc) {
        collapse_gpu_arch_args(edited);
    }

    auto command = base;
    command.query_toolchain = base.query_toolchain && edits.query_toolchain;
    // A rule may take `--driver-mode=cl` out of a command, or put it in:
    // the family follows the edited command's last driver mode, as it
    // follows the loaded one's.
    command.family = driver_family(base.driver);
    for(auto& local: llvm::reverse(edited)) {
        if(local.opt_id == option::OPT_driver_mode && local.values.size() == 1) {
            if(llvm::StringRef(local.values[0]) == "cl") {
                command.family = CompilerFamily::ClangCL;
            }
            break;
        }
    }
    return commands.save(command, edited);
}

}  // namespace clice
