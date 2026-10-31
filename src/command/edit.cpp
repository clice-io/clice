module clice;

import :command.argument;
import :command.edit;
import :command.normalize;
import :command.toolchains.nvcc;
import :command.table;
import :support.logging;

namespace clice {

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
        key += edit.directory;
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

    Normalizer normalizer{.commands = commands, .files = files};

    auto remove_parse_options =
        kotatsu::option::ParseOptions{.visibility = family_visibility(base.family)};

    /// One remove pattern: the option it names, and whether a `*` joined
    /// to the option's name takes every option spelled with that prefix
    /// (`-W*` takes -Wall and -Wextra, options of their own).
    struct RemovePattern {
        kotatsu::option::ParsedArg option;
        bool prefix = false;
    };

    /// Parse one rule's remove list into option patterns. The parsed
    /// patterns view the translated spellings, which therefore outlive
    /// every match below.
    std::deque<std::vector<std::string>> remove_storage;
    auto parse_removes = [&](llvm::ArrayRef<std::string> flags, const Spelling& anchor) {
        auto& remove_flags = remove_storage.emplace_back(
            is_nvcc ? translate_nvcc_removes(normalizer, base.driver, flags, anchor)
                    : std::vector<std::string>(flags.begin(), flags.end()));
        std::vector<RemovePattern> removes;
        auto add = [&](const kotatsu::option::ParsedArg& parsed, llvm::StringRef written) {
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
        /// spelling (NVCC query flags persist as unknown tokens). A
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
            std::erase_if(append_args, [&](const LocalArg& local) {
                return ranges::any_of(removes, [&](const RemovePattern& remove) {
                    return removes_arg(remove, local);
                });
            });
            remove_args.insert(remove_args.end(), removes.begin(), removes.end());
        } else {
            auto flags = is_nvcc ? translate_nvcc_flags(normalizer,
                                                        base.driver,
                                                        edit.flags,
                                                        edit.directory,
                                                        /*edit=*/true)
                                 : edit.flags;
            parse_edit(flags, edit.directory, append_args);
        }
    }
    auto entry_directory = Spelling::absolute(base.directory);
    parse_edit(edits.extra_append, entry_directory, append_args);

    std::vector<LocalArg> prepend_args;
    parse_edit(edits.extra_prepend, entry_directory, prepend_args);

    auto matches_remove = [&](const Arg& arg) {
        return ranges::any_of(remove_args, [&](const RemovePattern& remove) {
            return removes_arg(remove, arg);
        });
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
            .alias = arg.alias,
            .spelling = arg.spelling,
            .values = {arg.values.begin(), arg.values.end()}
        });
    }
    edited.append_range(append_args);
    edited.push_back({.opt_id = option::OPT_INPUT, .cls = ArgClass::Input});
    edited.append_range(trailing_selectors);

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
    for(auto& local: views::reverse(edited)) {
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
